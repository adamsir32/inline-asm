#include "native_kernel_fixture.hpp"

#include "hpu/delivery/application_package.hpp"
#include "hpu/delivery/resource_compiler.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#ifndef HPU_DELIVERY_REVISION
#define HPU_DELIVERY_REVISION "unknown"
#endif
#ifndef HPU_DELIVERY_TREE_STATE
#define HPU_DELIVERY_TREE_STATE "unknown"
#endif
#ifndef HPU_DELIVERY_SOURCE_DIR
#define HPU_DELIVERY_SOURCE_DIR ""
#endif

namespace {
struct Options {
    std::string kernel = "cmult";
    std::filesystem::path destination;
};

Options parse_options(int argc, char** argv)
{
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--case" && i + 1 < argc) result.kernel = argv[++i];
        else if (argument == "--emit-dir" && i + 1 < argc) result.destination = argv[++i];
        else throw std::invalid_argument("usage: hpu_native_kernel_application_example "
            "[--case cmult|ntt|intt] --emit-dir PATH");
    }
    if (result.destination.empty() ||
        (result.kernel != "cmult" && result.kernel != "ntt" && result.kernel != "intt"))
        throw std::invalid_argument("usage: hpu_native_kernel_application_example "
            "[--case cmult|ntt|intt] --emit-dir PATH");
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = parse_options(argc, argv);
        auto fixture = options.kernel == "cmult" ? native_kernel_example::cmult_fixture()
            : native_kernel_example::transform_fixture(options.kernel == "intt");
        auto request = native_kernel_example::package_request(fixture, options.kernel,
            hpu::delivery::verified_producer_identity("inline-asm",
                HPU_DELIVERY_REVISION, HPU_DELIVERY_TREE_STATE,
                HPU_DELIVERY_SOURCE_DIR));
        hpu::delivery::compile_application_resources(request, fixture.image);
        hpu::delivery::write_application_package(options.destination, request);
        std::cout << "Native " << options.kernel << " application package: "
                  << options.destination.string()
                  << "\nResource and verification details: oracle/report.json\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native kernel application failed: " << error.what() << '\n';
        return 1;
    }
}
