#include "evaluator_application.hpp"

int main(int argc, char** argv)
{
    return evaluator_example::integer_main(seal::scheme_type::bgv,
        "bgv_evaluator_application", argc, argv,
        [](auto& app, auto& evaluator, const auto& x, const auto& bias, const auto& relin, const auto& galois) {
            hpu::seal_adapter::Ciphertext rows, columns, mixed, y;
            evaluator.rotate_rows(x, 1, galois, rows);
            evaluator.rotate_columns(x, galois, columns);
            evaluator.add(rows, columns, mixed);
            app.mark_output("mixed", mixed);
            evaluator.multiply(x, mixed, y);
            evaluator.relinearize_inplace(y, relin);
            evaluator.mod_switch_to_next_inplace(y);
            evaluator.add_plain_inplace(y, bias);
            return y;
        });
}
