# ContextQL agent memory playbook

The canonical agent instructions now live in [README.md](README.md).

If you are an AI agent, read that file first. The short version:

- SkibidiQL is the context database.
- `spill-context` compiles the model-facing context pack.
- Put only `current_context` into the LLM prompt.
- Treat `invalidated` rows as receipts, not active facts.
- Append corrections and durable decisions back into SkibidiQL.

Do not paste the whole conversation into the model when SkibidiQL can query the
active context instead.
