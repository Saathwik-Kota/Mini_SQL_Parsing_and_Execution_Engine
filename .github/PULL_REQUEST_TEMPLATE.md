## Summary

<!-- Briefly explain the motivation, context, and changes introduced by this pull request. -->

## Subsystem(s) Affected

- [ ] Storage & Slotted Pages (`slotted_page.c`, `slotted_page.h`)
- [ ] Buffer Pool Manager (`buffer_pool.c`, `buffer_pool.h`)
- [ ] B+ Tree Indexing (`bplus_tree.c`, `bplus_tree.h`)
- [ ] System Catalog & Disk I/O (`catalog.c`, `disk_manager.c`)
- [ ] SQL Lexer & Parser (`parser.c`, `parser.h`)
- [ ] Rule-Based Planner (`planner.c`, `planner.h`)
- [ ] Volcano Execution Engine (`executor.c`, `executor.h`)
- [ ] REPL, Benchmarks, or Build System (`main.c`, `Makefile`, `.github/`)

## Key Changes

<!-- Provide a bulleted list of key code or structural modifications. -->
- 

## Verification & Testing

- [ ] Automated test suite passes (`make test` or `117/117 assertions passed`)
- [ ] Database benchmark passes without regression (`make benchmark`)
- [ ] Clean compilation with zero warnings under `-Wall -Wextra`
- [ ] Verified under AddressSanitizer and UndefinedBehaviorSanitizer
- [ ] Verified with Valgrind (zero memory leaks, zero errors: `make valgrind-test`)
- [ ] Formatted according to repository `.clang-format` standard

## Related Issues

<!-- Link any relevant issues here (e.g. Closes #12, Fixes #34) -->
