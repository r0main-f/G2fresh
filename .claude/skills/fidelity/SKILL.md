---
name: fidelity
description: Check a G2fresh feature or change against the original (the G2 with OS 1.62 and Clavia's editor v1.62) and list every difference with its evidence. Use before building a feature, after changing behaviour, or when the user asks whether something matches the original.
context: fork
agent: fidelity-checker
argument-hint: "<feature, menu item, message or file to check>"
---
Check this against the original, following your method, and report: $ARGUMENTS

If nothing is named above, check what the current branch changes (`git diff main...HEAD`, or `git diff HEAD` when on main): every change in behaviour it contains, one by one.
