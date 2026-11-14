; lmok facts extraction for C++ (tree-sitter-cpp v0.23.4).
; Vocabulary: `@def.<kind>` / `@ref.<kind>` on the fact node, `@name` on the
; name node; `(#set! confidence "...")` overrides the default confidence.
; Scope chains come from enclosing def.namespace / def.class / def.struct /
; def.union / def.enum / def.function / def.method / def.prototype anchors
; (cpp/facts.cpp). Named casts (static_cast, etc.) parse as
; template_function call_expressions and are explicitly excluded from ref.call.

; ---- definitions ----------------------------------------------------------
(namespace_definition name: (namespace_identifier) @name) @def.namespace
(namespace_definition name: (nested_namespace_specifier) @name) @def.namespace

(function_definition
  declarator: [
    (function_declarator declarator: [
      (identifier) @name
      (field_identifier) @name
      (destructor_name) @name
      (operator_name) @name
    ])
    (pointer_declarator declarator: (function_declarator declarator: [(identifier) @name (field_identifier) @name (operator_name) @name]))
    (pointer_declarator declarator: (pointer_declarator declarator: (function_declarator declarator: [(identifier) @name (field_identifier) @name])))
    (reference_declarator (function_declarator declarator: [(identifier) @name (field_identifier) @name (operator_name) @name]))
  ]) @def.function

; Out-of-line qualified definitions (`Ret Foo::bar()`, `Foo::~Foo()`, etc.):
; host normalizes nested qualifiers into `scope`, extracts the leaf `name`,
; and promotes syntactic class members to confidence "exact".
((function_definition
   declarator: [
     (function_declarator declarator: (qualified_identifier) @name)
     (pointer_declarator declarator: (function_declarator declarator: (qualified_identifier) @name))
     (pointer_declarator declarator: (pointer_declarator declarator: (function_declarator declarator: (qualified_identifier) @name)))
     (reference_declarator (function_declarator declarator: (qualified_identifier) @name))
   ]) @def.method
  (#set! confidence "candidate"))

; Function prototypes and class method declarations.
(declaration
  declarator: [
    (function_declarator declarator: [(identifier) @name (destructor_name) @name (operator_name) @name])
    (pointer_declarator declarator: (function_declarator declarator: [(identifier) @name (operator_name) @name]))
    (reference_declarator (function_declarator declarator: [(identifier) @name (operator_name) @name]))
  ]) @def.prototype

(field_declaration
  declarator: [
    (function_declarator declarator: [(field_identifier) @name (destructor_name) @name (operator_name) @name])
    (pointer_declarator declarator: (function_declarator declarator: [(field_identifier) @name (operator_name) @name]))
    (reference_declarator (function_declarator declarator: [(field_identifier) @name (operator_name) @name]))
  ]) @def.prototype

(preproc_def name: (identifier) @name) @def.macro
(preproc_function_def name: (identifier) @name) @def.macro

(class_specifier name: (type_identifier) @name body: (_)) @def.class
(struct_specifier name: (type_identifier) @name body: (_)) @def.struct
(union_specifier name: (type_identifier) @name body: (_)) @def.union
(enum_specifier name: (type_identifier) @name body: (_)) @def.enum
(enumerator name: (identifier) @name) @def.enumerator
(type_definition declarator: (type_identifier) @name) @def.typedef
(alias_declaration name: (type_identifier) @name) @def.type

; Variables, structured bindings, parameters, fields and labels.
(declaration
  declarator: [
    (identifier) @name
    (init_declarator declarator: [
      (identifier) @name
      (pointer_declarator declarator: [
        (identifier) @name
        (pointer_declarator declarator: (identifier) @name)
        (array_declarator declarator: (identifier) @name)
      ])
      (reference_declarator (identifier) @name)
      (array_declarator declarator: [
        (identifier) @name
        (pointer_declarator declarator: (identifier) @name)
      ])
      (structured_binding_declarator (identifier) @name)
    ])
    (pointer_declarator declarator: [
      (identifier) @name
      (pointer_declarator declarator: (identifier) @name)
      (array_declarator declarator: (identifier) @name)
    ])
    (reference_declarator (identifier) @name)
    (array_declarator declarator: [
      (identifier) @name
      (pointer_declarator declarator: (identifier) @name)
    ])
  ]) @def.variable

(parameter_declaration
  declarator: [
    (identifier) @name
    (pointer_declarator declarator: [
      (identifier) @name
      (pointer_declarator declarator: (identifier) @name)
    ])
    (reference_declarator [
      (identifier) @name
      (pointer_declarator declarator: (identifier) @name)
    ])
    (array_declarator declarator: (identifier) @name)
  ]) @def.parameter

(optional_parameter_declaration
  declarator: [
    (identifier) @name
    (pointer_declarator declarator: (identifier) @name)
    (reference_declarator (identifier) @name)
  ]) @def.parameter

(field_declaration
  declarator: [
    (field_identifier) @name
    (pointer_declarator declarator: (field_identifier) @name)
    (reference_declarator (field_identifier) @name)
    (array_declarator declarator: (field_identifier) @name)
  ]) @def.field

(labeled_statement label: (statement_identifier) @name) @def.label

; ---- references and edges -------------------------------------------------
(call_expression function: (identifier) @name) @ref.call
(call_expression
  function: (field_expression
    field: [
      (field_identifier) @name
      (destructor_name) @name
      (template_method name: (field_identifier) @name)
    ])) @ref.call
(call_expression function: (qualified_identifier) @name) @ref.call
((call_expression
   function: (template_function name: (identifier) @name)) @ref.call
 (#not-any-of? @name "static_cast" "dynamic_cast" "const_cast" "reinterpret_cast"))

(preproc_include path: (string_literal (string_content) @name)) @ref.include
((preproc_include path: (system_lib_string) @name) @ref.include
  (#set! confidence "external"))
