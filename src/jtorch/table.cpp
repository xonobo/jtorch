#include <sstream>
#include <iostream>
#include <stdexcept>
#include <fstream>
#include "jtorch/table.h"
#include "jcl/data_str/vector_managed.h"

#define SAFE_DELETE(x) if (x != NULL) { delete x; x = NULL; }
#define SAFE_DELETE_ARR(x) if (x != NULL) { delete[] x; x = NULL; }

namespace jtorch {

  Table::Table() {
    data_ = new jcl::data_str::VectorManaged<TorchData*>();
  }

  Table::~Table() {
    delete data_;
  }

  TorchData* Table::operator()(const uint32_t i) {
    return (*data_)[i];
  }

  void Table::print() {
    for (uint32_t i = 0; i < data_->size(); i++) {
      std::cout << "Table[" << i << "] = " << std::endl;
      (*data_)[i]->print();
    }
  };

  void Table::add(TorchData* new_data) {
    data_->pushBack(new_data);
  }

  void Table::clearNoDelete() {
    for (uint32_t i = 0; i < data_->size(); i++) {
      (*data_)[i] = NULL;
    }
    data_->clear();
  }

  uint32_t Table::tableSize() const {
    return data_->size();
  }

}  // namespace jtorch