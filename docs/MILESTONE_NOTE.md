# Batched Dynamic Hierarchy Updates: Current Milestone

I replaced the exact transitive-closure path with a separate sparse C++ engine for the large experiments. The old implementation is still used as the oracle on small graphs.

The sparse engine stores the hierarchy in CSR form, checks all add/delete preconditions, merges a batch once, and validates the resulting DAG. I cross-checked it against the exact model on hand-written cases and 200 deterministic random cases. The earlier exhaustive five-node and independent Python checks also still pass.

I then ran the requested range: 1, 5, and 10 million nodes, with update batches of 1%, 5%, and 10%. The main 10-million-node graph has 11,249,915 edges and each CSR state uses about 125 MB. With full DAG validation, applying 1 million updates took a median 190.445 ms over five runs on my machine. I am treating this only as a preliminary scaling result because it is not integrated with DomLock metadata yet.

The more useful observation is about batch partitioning. I added the actual dominator tree rather than using the primary-tree LCA as a substitute. One dominator for the whole batch selected the root in every configuration. When shared-parent edges remained inside a top-level branch, local update pairs selected the root 0% of the time, compared with 31--37% for global pairs. However, when shared-parent edges crossed branches, even local pairs selected the root about 49--53% of the time. So primary-tree locality is only a filter; the partition key has to use the actual dominator, and cross-branch groups need escalation or a different repair path.

I also traced the supplied STMBench source. Its assembly/component containment hierarchy and its cyclic atomic-part connection graph should not be treated as the same edge relation. I have documented the adapter boundary before modifying the legacy artifact.

The next implementation step is metadata delta repair over the affected region, followed by the DomLock adapter and parallel group preparation.
