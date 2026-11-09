# v0.7.18 tests

Each defect test is written to FAIL against the v0.7.17 code and pass against
v0.7.18, so it demonstrates the bug rather than only asserting the fix.

    gcc -O1 -o test_fixes test_fixes.c && ./test_fixes        # 10 checks
    python3 test_server.py                                     # 15 checks
    python3 test_web.py        # needs playwright + chromium   # 34 checks
    python3 test_delivery.py   # needs playwright + chromium   #  5 checks

test_pmm.c and test_chrome.c compile the real kernel sources natively against
stubs; see the header comment in each for the small shim they expect.
