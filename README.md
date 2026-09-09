# GodByte++

GodByte++ is a random systems programming language i built for learning and exploring compilers. (its called ++ because i made like multiple versions already)

It is a compiled language with a custom frontend, IR, and backend, targeting native execution.

## Components

* **gbpp** - main compiler (lexer, parser, IR, backend)
* **divo** - build tool for GodByte++ projects

## Language Documentation

* [Language Rules](LANGUAGE_RULES.md) - syntax

## Dependencies

This project includes third-party libraries:

* nlohmann/json (MIT License) // i dont even use this anymore tho
* toml++ (MIT License)

These libraries are used as-is and retain their original license notices in their headers.

## License

GNU V3.0

## Other Info

if you write code in gbpp you can keep it closed source, and any binaries compiled using divo can also be closed source
the compiler itself is open source, so if you want to fork it and mess aorund with your own language rules, it needs to be open source and comply with GNU V3.0
