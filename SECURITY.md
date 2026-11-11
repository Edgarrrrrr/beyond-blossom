# Security

Report vulnerabilities privately through the repository's security advisory channel when available. Otherwise contact the maintainer through the GitHub profile linked from the repository before disclosing exploit details in a public issue.

Include the affected revision, build configuration, minimal input, expected behavior, and observed failure. Avoid including credentials or unpublished user data.

The decoder is a native library. Inputs must satisfy documented shape and graph constraints; use separate decoder instances for concurrent calls. Sanitizer builds are available through `BB_SANITIZERS=ON`.
