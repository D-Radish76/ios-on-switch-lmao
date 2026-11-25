# seed_cache

Puts a download Bazel cannot finish into its repository cache. Bazel 9.3.1 does not retry a read timeout in the body
of a download (bazelbuild/bazel#32132), and a dropped connection inside a `Range` request fails with `Multiple entries
with same key` because the retry adds a second `Range` header. Both leave an IPSW member or the LLVM archive unfetched.

Entries are stored under their sha256, so a repository rule that downloads the same sha256 finds it and does not
touch the network. Bytes that do not match the pin are never stored.

The steps are in `BUILD_CHECKLIST.md`.
