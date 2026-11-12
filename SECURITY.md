# Security Policy

## Reporting a Vulnerability

If you discover a security or privacy issue in **fcitx5-shadowbar** (such as unsafe subprocess execution, privilege escalation in package installation helpers, config/clipboard injection, or credential exposure), please avoid posting exploit details in public issues first:

- Open a GitHub **Private Vulnerability Report** (Security Advisories) at:
  `https://github.com/ang-XWBWZ/fcitx5-shadowbar/security/advisories`
- Or contact maintainer **ang** (`ang-XWBWZ`) via GitHub.

## Privacy & Credential Practices

- **Local API Key Storage**: Configuration lives at `~/.config/shadowbar/config.toml`. You can reference environment variables via `api_key = "env:YOUR_VAR"` instead of saving plaintext secrets on disk.
- **Clipboard Safeguards**: The floating window clipboard monitor actively skips password/sensitive MIME hints (e.g., password managers), local file paths/URIs, HTTP(S)/mailto URLs, Markdown links, and binary non-text data.
- **Local Package Elevation**: The built-in Chinese Environment & IME Fixer installs system packages only after user confirmation via `pkexec` / `sudo` terminal prompts and never stores elevated credentials.
- **No Telemetry**: `fcitx5-shadowbar` collects no telemetry. Network requests occur only when invoking an online translation provider you select or when manually downloading a Hy-MT2 model or `llama-server` engine from HuggingFace / HF-Mirror / GitHub Releases.
