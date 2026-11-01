# Security

Cstone 0.2.0 is an assembler MVP. It parses text and produces bytes; it does not execute the result, load plugins, inspect processes or contact the network.

The bounded API validates source/output sizes, symbol counts, statement counts, address ranges and operand delimiters. Failed assembly returns no partial allocations. External linking is transactional. Tests include allocation-failure injection, sanitizers and malformed inputs. These checks are finite and do not constitute a formal proof or an independent security audit.

Callers must supply valid C pointers and buffer lengths, retain ownership rules, select a matching CPU profile and apply their own service quotas. Do not share a mutable result across threads without synchronization.

## Reporting

Use the repository's Security tab if private vulnerability reporting is available. If no private reporting option is shown, open an issue requesting a private contact without including vulnerability details, credentials, or sensitive data. A verified private reporting channel is still a prerequisite for a stable production release.

## Version support

Version 0.2.x is an MVP and has no security-support SLA. Report the affected version or commit, host platform, target profile, and a minimal reproducer through the agreed private channel. The 0.1 prototype is superseded by 0.2 and is not ABI-compatible.
