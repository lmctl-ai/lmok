; Java syntax facts. Calls are candidates; imports preserve their source target.
(class_declaration name: (identifier) @name) @def.class
(interface_declaration name: (identifier) @name) @def.interface
(enum_declaration name: (identifier) @name) @def.enum
(record_declaration name: (identifier) @name) @def.class
(method_declaration name: (identifier) @name) @def.method
(constructor_declaration name: (identifier) @name) @def.method
(field_declaration declarator: (variable_declarator name: (identifier) @name) @def.field)
(formal_parameter name: (identifier) @name) @def.parameter
; `import a.b.C;` binds C; `import a.b.*;` (trailing asterisk) binds no
; single name, so the anchor requires the scoped_identifier to be last.
(import_declaration (scoped_identifier name: (identifier) @name) .) @def.import
(import_declaration (scoped_identifier) @name) @ref.import
(import_declaration (identifier) @name) @ref.import
(method_invocation name: (identifier) @name) @ref.call
(field_access field: (identifier) @name) @ref.read
