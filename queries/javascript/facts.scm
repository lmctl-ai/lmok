; lmok facts extraction for JavaScript: declaration names, imports and calls.

(function_declaration name: (identifier) @name) @def.function
(method_definition name: (property_identifier) @name) @def.method
(class_declaration name: (identifier) @name) @def.class
(variable_declarator name: (identifier) @name) @def.variable
(import_clause (identifier) @name) @def.import
(namespace_import (identifier) @name) @def.import
; `import { a as b }` binds b only: the source name is not a local binding.
(import_specifier alias: (identifier) @name) @def.import
(import_specifier !alias name: (identifier) @name) @def.import

(import_statement source: (string) @name) @ref.import
(export_statement source: (string) @name) @ref.import
(call_expression function: (identifier) @name) @ref.call
(call_expression function: (member_expression property: (property_identifier) @name)) @ref.call
(new_expression constructor: (identifier) @name) @ref.call
