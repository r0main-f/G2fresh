---
name: check
description: Build G2fresh and run everything that can be run here - unit tests, the tests on the user's G2 OS, the plugin in a test host - and report what passed, failed and was not run. Use before a pull request or when the user asks whether the tests pass. This is the test run; watching the running app is /verify.
context: fork
agent: tester
argument-hint: "[what changed, to look at first]"
---
Test the current checkout as your instructions describe, all six steps, and report.

What changed, if the user said: $ARGUMENTS

If that names an area, run its tests first and say so in the report (for example the USB transport: `build/tests/g2usbtests`; the emulator: the firmware tests; the plugin's audio: `g2plugintest` and its `--inputs` and `--outputs` modes), then run everything else as usual.
