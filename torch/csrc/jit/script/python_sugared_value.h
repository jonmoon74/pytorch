#pragma once

#include <torch/csrc/jit/pybind_utils.h>
#include <torch/csrc/jit/script/module.h>
#include <torch/csrc/jit/script/sugared_value.h>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace torch {
namespace jit {
namespace script {

std::string typeString(py::handle h);

inline std::shared_ptr<SugaredValue> toSimple(Value* v) {
  return std::make_shared<SimpleValue>(v);
}

// NB: This should be the single entry-point for instantiating a SugaredValue
// from a Python object. If you are adding support for converting a new Python
// type, *add it in this function's implementation*.
std::shared_ptr<SugaredValue> toSugaredValue(
    py::object obj,
    Function& m,
    SourceRange loc,
    bool is_constant = false);

c10::optional<StrongFunctionPtr> as_function(const py::object& obj);

struct VISIBILITY_HIDDEN PythonValue : public SugaredValue {
  PythonValue(
      py::object the_self,
      c10::optional<py::object> rcb = c10::nullopt,
      Value* module_self = nullptr)
      : self(std::move(the_self)), rcb(std::move(rcb)), moduleSelf_(module_self) {}

  FunctionSchema getSchema(
      const size_t n_args,
      const size_t n_binders,
      const SourceRange& loc);

  // call it like a function, e.g. `outputs = this(inputs)`
  std::shared_ptr<SugaredValue> call(
      const SourceRange& loc,
      Function& m,
      at::ArrayRef<NamedValue> inputs_,
      at::ArrayRef<NamedValue> attributes,
      size_t n_binders) override;

  std::string kind() const override;

  std::vector<std::shared_ptr<SugaredValue>> asTuple(
      const SourceRange& loc,
      Function& m,
      const c10::optional<size_t>& size_hint = {}) override;

  std::shared_ptr<SugaredValue> attr(
      const SourceRange& loc,
      Function& m,
      const std::string& field) override;

 protected:
  py::object getattr(const SourceRange& loc, const std::string& name);

  void checkForAddToConstantsError(std::stringstream& ss);

  py::object self;
  c10::optional<py::object> rcb;
  Value* moduleSelf_ = nullptr;
};

struct VISIBILITY_HIDDEN PythonModuleValue : public PythonValue {
  explicit PythonModuleValue(py::object mod) : PythonValue(std::move(mod)) {}

  std::shared_ptr<SugaredValue> attr(
      const SourceRange& loc,
      Function& m,
      const std::string& field) override;
};

struct VISIBILITY_HIDDEN ConstantPythonTupleValue : public PythonValue {
  explicit ConstantPythonTupleValue(py::object tup)
      : PythonValue(std::move(tup)) {}
  std::vector<std::shared_ptr<SugaredValue>> asTuple(
      const SourceRange& loc,
      Function& m,
      const c10::optional<size_t>& size_hint = {}) override;

  Value* asValue(const SourceRange& loc, Function& m) override;
};

// Represents all the parameters of a module as a List[Tensor]
struct VISIBILITY_HIDDEN ConstantParameterList : public SugaredValue {
  ConstantParameterList(Value* the_list) : the_list_(the_list) {}
  std::string kind() const override {
    return "constant parameter list";
  }
  std::shared_ptr<SugaredValue> call(
      const SourceRange& loc,
      Function& caller,
      at::ArrayRef<NamedValue> inputs,
      at::ArrayRef<NamedValue> attributes,
      size_t n_binders) override {
    return toSimple(the_list_);
  }

 private:
  Value* the_list_;
};

struct VISIBILITY_HIDDEN ConstantTupleValue : public SugaredValue {
  explicit ConstantTupleValue(
      std::vector<std::shared_ptr<SugaredValue>> tup,
      bool callable = false)
      : tup_(tup){};

  std::vector<std::shared_ptr<SugaredValue>> asTuple(
      const SourceRange& loc,
      Function& m,
      const c10::optional<size_t>& size_hint = {}) override {
    return tup_;
  };

  std::string kind() const override {
    return "constant tuple";
  }

  std::vector<std::shared_ptr<SugaredValue>> tup_;
  bool callable_;
};

struct VISIBILITY_HIDDEN ConstantTupleMethod : public SugaredValue {
  explicit ConstantTupleMethod(
      std::vector<std::shared_ptr<SugaredValue>> tup,
      const std::string& name)
      : tup_(tup), name_(name){};

  std::string kind() const override {
    return name_;
  }

  std::shared_ptr<SugaredValue> call(
      const SourceRange& loc,
      Function& f,
      at::ArrayRef<NamedValue> inputs,
      at::ArrayRef<NamedValue> attributes,
      size_t n_binders) override {
    if (inputs.size() || attributes.size()) {
      throw ErrorReport(loc)
          << name_ << " method does not accept any arguments";
    }
    return std::make_shared<ConstantTupleValue>(tup_);
  }

  std::vector<std::shared_ptr<SugaredValue>> tup_;
  const std::string name_;
};

struct VISIBILITY_HIDDEN OverloadedMethodValue : public SugaredValue {
  OverloadedMethodValue(Value* module, std::vector<std::string> method_names)
      : module_(module), method_names_(std::move(method_names)) {}

  std::string kind() const override {
    return "overloaded function";
  }

  std::shared_ptr<SugaredValue> call(
      const SourceRange& loc,
      Function& caller,
      at::ArrayRef<NamedValue> inputs,
      at::ArrayRef<NamedValue> attributes,
      size_t n_binders) override;

 private:
  Value* module_;
  std::vector<std::string> method_names_;
};

struct VISIBILITY_HIDDEN OverloadedFunctionValue : public SugaredValue {
  OverloadedFunctionValue(std::vector<StrongFunctionPtr> compiled_overloads)
      : compiled_overloads_(std::move(compiled_overloads)) {}

  std::string kind() const override {
    return "overloaded function";
  }

  std::shared_ptr<SugaredValue> call(
      const SourceRange& loc,
      Function& caller,
      at::ArrayRef<NamedValue> inputs,
      at::ArrayRef<NamedValue> attributes,
      size_t n_binders) override;

 private:
  std::vector<StrongFunctionPtr> compiled_overloads_;
};

// You can think of an nn.Module as a template that corresponds to a family of
// JIT types. The template "arguments" are things like the constant values.
// e.g.
//   class M(nn.Module):
//        __constants__ = ["const"]
//        ...
//
// Is similar to writing the following in C++:
//
//    template<TConst>
//    class M {
//       ...
//    }
//
// We need to consider each different member of the type family a different JIT
// type because, e.g. different constant values lead to different versions of
// the same method.
//
// ConcreteModuleType corresponds to a single member of the type family, with
// all template arguments fully specified. Two Modules that share a
// ConcreteModuleType can share a JIT type, and vice versa.
//
// Why not just use a JIT type to represent concrete types? Because constants,
// function attributes, etc. are currently not representable in the type system,
// so this acts a non-first-class way of tracking concrete types.
//
// ConcreteModuleType is also the source of truth for servicing all
// ModuleValue::attr calls. This is so we can guarantee that if two Module's
// share a JIT type (and thus a ConcreteModuleType), then they behave the same
// way when you access attributes on them.
struct VISIBILITY_HIDDEN ConcreteModuleType {
  ClassTypePtr jitType() const {
    TORCH_INTERNAL_ASSERT(jitType_);
    return jitType_;
  }

  ClassTypePtr createNewTypeFromThis() {
    TORCH_INTERNAL_ASSERT(!jitType_);
    TORCH_INTERNAL_ASSERT(pyClass_);

    auto cu = get_python_cu();
    py::object pyQualName = py::module::import("torch._jit_internal")
                                .attr("_qualified_name")(pyClass_);

    auto className = c10::QualifiedName(py::cast<std::string>(pyQualName));
    if (className.prefix().empty()) {
      className = c10::QualifiedName("__torch__", className.name());
    }
    if (cu->get_class(className) != nullptr) {
      className = cu->mangle(className);
    }
    auto cls = ClassType::create(std::move(className), cu, /*is_module=*/true);
    cu->register_type(cls);

    // populate type with info from the concrete type information
    for (const auto& pr : attributes_) {
      const auto& name = pr.first;
      const auto& type = pr.second.type_;
      const auto& isParameter = pr.second.isParam_;

      cls->addAttribute(name, type, isParameter);
    }

    jitType_ = std::move(cls);
    return jitType_;
  }

  void addJitType(ClassTypePtr type) {
    TORCH_INTERNAL_ASSERT(!jitType_)
    jitType_ = std::move(type);
  }

  void addPyClass(py::object pyClass) {
    TORCH_INTERNAL_ASSERT(!jitType_);
    pyClass_ = std::move(pyClass);
  }

  void addConstant(std::string name, py::object value) {
    TORCH_INTERNAL_ASSERT(!jitType_);
    constants_.emplace(std::move(name), std::move(value));
  }

  void addAttribute(std::string name, TypePtr type, bool isParameter) {
    TORCH_INTERNAL_ASSERT(type);
    TORCH_INTERNAL_ASSERT(!jitType_);
    if (auto functionType = type->cast<FunctionType>()) {
      functionAttributes_.emplace(std::move(name), std::move(functionType));
    } else {
      attributes_.emplace(
          std::move(name), Attribute(unshapedType(type), isParameter));
    }
  }

  void addModule(
      std::string name,
      TypePtr type,
      std::shared_ptr<ConcreteModuleType> meta) {
    TORCH_INTERNAL_ASSERT(type);
    TORCH_INTERNAL_ASSERT(!jitType_);
    modules_.emplace_back(
        ModuleInfo{std::move(name), std::move(type), std::move(meta)});
  }

  void addOverload(
      std::string methodName,
      std::vector<std::string> overloadedMethodNames) {
    TORCH_INTERNAL_ASSERT(!jitType_);
    overloads_.emplace(std::move(methodName), std::move(overloadedMethodNames));
  }

  c10::optional<py::object> findConstant(const std::string& name) const {
    auto it = constants_.find(name);
    if (it != constants_.end()) {
      return it->second.v_;
    }
    return c10::nullopt;
  }

  // This determines whether two modules can share a type. The container structs
  // used by ConcreteModuleType have been defined such that operator==
  // implements a meaningful comparison in that context.
  friend bool operator==(
      const ConcreteModuleType& lhs,
      const ConcreteModuleType& rhs) {
    return lhs.pyClass_.is(rhs.pyClass_) && lhs.constants_ == rhs.constants_ &&
        lhs.attributes_ == rhs.attributes_ && lhs.modules_ == rhs.modules_ &&
        lhs.overloads_ == rhs.overloads_ &&
        lhs.functionAttributes_ == rhs.functionAttributes_;
  }

  std::shared_ptr<ConcreteModuleType> findSubmoduleConcreteType(
      const std::string& name) const {
    const auto it = std::find_if(
        modules_.cbegin(), modules_.cend(), [&](const ModuleInfo& info) {
          return info.name == name;
        });
    if (it == modules_.end()) {
      return nullptr;
    }
    return it->meta;
  }

  struct Constant {
    /* implicit */ Constant(py::object v) : v_(std::move(v)) {}
    friend bool operator==(const Constant& lhs, const Constant& rhs) {
      // Perform the equivalent of `lhs == rhs` in Python.
      int rv = PyObject_RichCompareBool(lhs.v_.ptr(), rhs.v_.ptr(), Py_EQ);
      if (rv == -1) {
        throw py::error_already_set();
      }
      return rv == 1;
    }
    py::object v_;
  };

  struct Attribute {
    Attribute(TypePtr type, bool isParam)
        : type_(std::move(type)), isParam_(isParam) {}

    friend bool operator==(const Attribute& lhs, const Attribute& rhs) {
      return *(lhs.type_) == *(rhs.type_) && lhs.isParam_ == rhs.isParam_;
    }
    TypePtr type_;
    bool isParam_;
  };

  struct ModuleInfo {
    std::string name;
    TypePtr type;
    std::shared_ptr<ConcreteModuleType> meta;

    friend bool operator==(const ModuleInfo& lhs, const ModuleInfo& rhs) {
      return *(lhs.type) == *(rhs.type) && lhs.name == rhs.name;
    }
  };

  // The value of any constants defined by the module.
  std::unordered_map<std::string, Constant> constants_;
  // The types of any attributes
  std::unordered_map<std::string, Attribute> attributes_;
  // Overloads, in the same format as `__overloads__` in Python
  std::unordered_map<std::string, std::vector<std::string>> overloads_;
  // Any attributes we failed to convert to TorchScript, along with a hint as to why
  std::unordered_map<std::string, std::string> failedAttributes_;
  // Any function attributes. These are special right now because functions are
  // not first-class in the type system.
  std::unordered_map<std::string, FunctionTypePtr> functionAttributes_;
  // The concrete types of any submodules
  std::vector<ModuleInfo> modules_;
  // The original `nn.Module` class that we derived this ScriptModule from.
  py::object pyClass_;
  ClassTypePtr jitType_ = nullptr;
};

struct VISIBILITY_HIDDEN ModuleValue : public SugaredValue {
  ModuleValue(Value* self, Module module, ConcreteModuleType concreteType)
      : self_(self),
        module_(std::move(module)),
        concreteType_(std::move(concreteType)) {}

  std::string kind() const override {
    return "module";
  }

  Value* asValue(const SourceRange& loc, Function& m) override;

  // select an attribute on it, e.g. `this.field`
  std::shared_ptr<SugaredValue> attr(
      const SourceRange& loc,
      Function& m,
      const std::string& field) override;

  // call module.forward
  std::shared_ptr<SugaredValue> call(
      const SourceRange& loc,
      Function& caller,
      at::ArrayRef<NamedValue> inputs,
      at::ArrayRef<NamedValue> attributes,
      size_t n_binders) override {
    return attr(loc, caller, "forward")
        ->call(loc, caller, inputs, attributes, n_binders);
  }

  std::vector<std::shared_ptr<SugaredValue>> asTuple(
      const SourceRange& loc,
      Function& m,
      const c10::optional<size_t>& size_hint = {}) override;

  void setAttr(
      const SourceRange& loc,
      Function& m,
      const std::string& field,
      Value* newValue) override;

 private:
  std::vector<std::shared_ptr<SugaredValue>> desugarModuleContainer(
      bool get_keys,
      bool get_values,
      const SourceRange& loc,
      Function& m);
  Value* self_;
  Module module_;
  ConcreteModuleType concreteType_;
};

struct VISIBILITY_HIDDEN BooleanDispatchValue : public SugaredValue {
  BooleanDispatchValue(py::dict dispatched_fn)
      : dispatched_fn_(std::move(dispatched_fn)) {}

  std::string kind() const override {
    return "boolean dispatch";
  }

  std::shared_ptr<SugaredValue> call(
      const SourceRange& loc,
      Function& caller,
      at::ArrayRef<NamedValue> inputs,
      at::ArrayRef<NamedValue> attributes,
      size_t n_binders) override;

 private:
  py::dict dispatched_fn_;
};

} // namespace script
} // namespace jit
} // namespace torch
