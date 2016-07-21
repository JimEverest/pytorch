#include <iostream>
#include <string>

#include "caffe2/core/init.h"
#include "caffe2/core/operator.h"
#include "caffe2/core/operator_schema.h"

DEFINE_string(schema, "", "Print doc and schema of a particular operator");

static bool HasSchema(const std::string& str) {
  return caffe2::OpSchemaRegistry::Schema(str);
}

static bool HasDoc(const std::string& str) {
  const auto* schema = caffe2::OpSchemaRegistry::Schema(str);
  return (schema != nullptr) && (schema->doc() != nullptr);
}

int main(int argc, char** argv) {
  caffe2::GlobalInit(&argc, &argv);

  if (!FLAGS_schema.empty()) {
    const auto* schema = caffe2::OpSchemaRegistry::Schema(FLAGS_schema);
    if (!schema) {
      std::cerr << "Operator " << FLAGS_schema << " doesn't have a schema"
                << std::endl;
      return 1;
    }
    std::cout << "Operator " << FLAGS_schema << ": " << std::endl << *schema;
    return 0;
  }

  std::cout << "CPU operator registry:" << std::endl;
  for (const auto& key : caffe2::CPUOperatorRegistry()->Keys()) {
    std::cout << "\t(schema: " << HasSchema(key) << ", doc: "
              << HasDoc(key) << ")\t"
              << key << std::endl;
  }
  std::cout << "CUDA operator registry:" << std::endl;
  for (const auto& key : caffe2::CUDAOperatorRegistry()->Keys()) {
    std::cout << "\t(schema: " << HasSchema(key) << ", doc: "
              << HasDoc(key) << ")\t"
              << key << std::endl;
  }
  std::cout << "Operators that have gradients registered:" << std::endl;
  for (const auto& key : caffe2::GradientRegistry()->Keys()) {
    std::cout << "\t(schema: " << HasSchema(key) << ", doc: "
              << HasDoc(key) << ")\t"
              << key << std::endl;
  }
  return 0;
}
