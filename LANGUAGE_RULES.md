# primitive types
- u8 - u64
- i8 - i64
- f16 - f64
- boolean
- void

# Type Modifyers
type modifyers follow a left to right order.
- const    // marks the following type as constant
- heap     // heap allocated
- ref      // pointer (heap || stack)
- volatile // perserves loads / stores
- variadic // instead of using "..." i am using variadic for now because sometimes "..." looks messy

so:
    `const ref u8`- > this is a const pointer, pointing to a u8.
    `const ref ref const u8` -> this is a constant pointer to a pointer, pointing at a const u8

