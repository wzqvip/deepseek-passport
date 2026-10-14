<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Fork Assets and Design Notes

This directory holds content that belongs to this fork rather than to the upstream
project: product design, architecture notes, requirements, and project-specific
assets. Upstream keeps it empty except for a placeholder, and nothing here is
proposed back upstream.

## Rules for this directory

- Every maintained document exists as a paired English `.md` and Simplified
  Chinese `.zh_CN.md` file with reciprocal language switches, exactly like the
  rest of the repository.
- Only fork-private material belongs here. Anything that would help any AI
  Passport user goes upstream instead.
- Never place credentials, tokens, device QR secrets, or unsanitized logs here.
- When a document is added, moved, or removed, update the index below and the
  root [readme](../../README.md) in the same change.

## Index

| Document | Contents |
| --- | --- |
| [Requirements](deepseek-passport/requirements.md) | What the product owner asked for, decomposed into requirements, assumptions, acceptance criteria, and open questions |
| [Architecture](deepseek-passport/architecture.md) | Hardware constraints, the memory budget, firmware and bridge architecture, the voice pipeline, agent integration, provisioning, the web console, and the pixel UI |
| [Voice back ends](deepseek-passport/voice-backends.md) | The provider survey behind the voice pipeline decision, and the rejected designs |
| [Roadmap](deepseek-passport/roadmap.md) | Milestones, exit criteria, and the validation plan |
| [Bridge protocol](deepseek-passport/bridge-protocol.md) | The contract between the device and its companion bridge: transport, the status document, the answer, and what the device deliberately does not do |
| [Progress log](deepseek-passport/progress-log.md) | Dated decisions, evidence, verification status, and open items |
| [Serial console](deepseek-passport/serial-cli.md) | The serial console: how screen elements and console commands stay in correspondence, the command list, and its contracts |

## Related

- Root readme: [README.md](../../README.md)
- Fork workflow and what is fork-owned: [fork guide](../fork-guide.md)
- Upstream project overview: [docs/README.md](../README.md)
