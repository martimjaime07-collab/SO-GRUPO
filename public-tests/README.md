# Public tests

These public tests build `cloudIST` and run a small scenario for each exercise.
The simulator output is shown as the test runs, followed by `Exercise N:
PASSED` or `Exercise N: FAILED`. On failure, the runner prints a short
explanation of the failed check; expected-output differences are reported
without printing a diff.

The expected files contain only output relevant to each test, not complete
transcripts. Exercise 1 checks that all three VM definitions and reservations
succeed. Exercise 2 checks that its VM is `RUNNING` in the first listing and
that the final listing contains no hosted VMs or reservations; its worker also
checks a nested input file using a relative path. If that file cannot be found,
the runner reports the worker's current directory. Exercise 3 checks the
expected reservations and one `DATA CENTER STATUS` marker per config. It sorts
the selected output before comparison because concurrent threads can print in
different orders.

The expected files omit unrelated resource counts and listing formatting, so
those details can change without breaking these focused checks.

## Running the tests

Run them from the repository root:

```sh
./public-tests/ex1/run.sh
./public-tests/ex2/run.sh
./public-tests/ex3/run.sh
```

Install `cpulimit` before running exercise 2, which launches a VM.

The scripts use half of the CPU cores available to the process by default
(minimum one), via `public-tests/common.sh`. Set `CLOUDIST_TEST_CORE_PERCENT` to
choose another percentage, for example:

```sh
CLOUDIST_TEST_CORE_PERCENT=25 ./public-tests/ex2/run.sh
```
