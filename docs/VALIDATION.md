# Validation against VOSviewer

VOSStudio aims to give the same maps as VOSviewer for the same network, so that results can be reported and
reproduced with either tool. This document describes how that was checked for version 1.6.0, what was found, and
what was changed as a result.

Reference: **VOSviewer 1.6.21** (June 2026), run headless from its command line.

## Method

1. **Same input.** Each network was exported from VOSStudio as a VOSviewer map file (ids and labels only, no
   coordinates or clusters) plus a VOSviewer network file. Both programs therefore start from identical items and link
   weights, and the comparison covers only layout and clustering, not network construction. VOSviewer can only import
   bibliographic database files through its wizard, so construction cannot be compared in batch.
2. **Same settings.** Association strength normalisation, attraction 2, repulsion 1, resolution 1, VOSviewer defaults
   otherwise (small clusters merged, minimum cluster size 1).
   - VOSviewer: `-map in.txt -network net.txt -run_layout -run_clustering -attraction 2 -repulsion 1 -save_map out.txt`
   - VOSStudio: the same engine the app uses, through a small command-line harness.
3. **Measures.**
   - *Layout:* Procrustes R² after the best rotation, reflection, scaling and translation, and the Pearson and
     Spearman correlations of all pairwise distances. A value of 1.000 means the maps are the same up to rotation,
     reflection and scale.
   - *Clustering:* adjusted Rand index (ARI) and normalised mutual information (NMI) between the two partitions,
     plus the quality of each partition under the VOS clustering quality function (below) and under modularity.

The harness (`validation/`) contains the export and run tool, the comparison scripts and the result table.
VOSviewer itself is not included; download it from vosviewer.com.

## Networks

| Network | Items | Links | Source |
|---|---:|---:|---|
| kw | 48 | 478 | Sample data, keyword co-occurrence |
| terms | 106 | 2,281 | Sample data, title/abstract terms |
| cite-docs | 74 | 101 | Sample data, document citation |
| coupling-docs | 98 | 1,827 | Sample data, bibliographic coupling |
| coauth | 21 | 47 | Sample data, co-authorship |
| cocit-sources | 19 | 147 | Sample data, source co-citation |
| journal | 232 | 4,112 | VOSviewer's own journal example network |
| oa-kw | 275 | 10,390 | 1,000 OpenAlex works, keyword co-occurrence |
| oa-terms | 914 | 104,311 | 1,000 OpenAlex works, terms |
| oa-coupling-docs | 879 | 142,591 | 1,000 OpenAlex works, bibliographic coupling |

## Results (1.6.0)

| Network | Layout R² | ARI | NMI | Clusters (VOSviewer / VOSStudio) | VOS quality (VOSviewer / VOSStudio) |
|---|---:|---:|---:|---|---|
| kw | 1.000 | 1.000 | 1.000 | 4 / 4 | 626.69 / 626.69 |
| terms | 1.000 | 1.000 | 1.000 | 4 / 4 | 3,885.49 / 3,885.49 |
| cite-docs | 1.000 | 1.000 | 1.000 | 11 / 11 | 2,018.39 / 2,018.39 |
| coupling-docs | 1.000 | 1.000 | 1.000 | 4 / 4 | 2,491.93 / 2,491.93 |
| coauth | 1.000 | 1.000 | 1.000 | 3 / 3 | 140.11 / 140.11 |
| cocit-sources | 1.000 | 1.000 | 1.000 | 4 / 4 | 77.29 / 77.29 |
| journal | 1.000 | 1.000 | 1.000 | 6 / 6 | 16,329.72 / 16,329.72 |
| oa-kw | 1.000 | 0.971 | 0.969 | 9 / 8 | 19,800.55 / **19,822.44** |
| oa-terms | 1.000 | 0.464 | 0.455 | 5 / 6 | **55,935.74** / 55,813.37 |
| oa-coupling-docs | 0.970 | 0.839 | 0.820 | 9 / 10 | 183,956.67 / **184,166.37** |

**Layouts** are identical on 9 of 10 networks (R² 1.000; VOSStudio coordinates are VOSviewer's multiplied by a
constant, with the same orientation). On the largest network (142,591 links) both layouts are local optima of the same
objective and still agree closely (R² 0.970, distance correlation 0.988).

**Clusters** are identical on the seven smaller networks. On the three large OpenAlex networks, both programs are
running a randomised heuristic (Leiden) on the same objective, and neither is always better. VOSStudio's partition
scores higher on VOSviewer's own objective for oa-kw and oa-coupling-docs, and lower for oa-terms.

**oa-terms** has very weak structure (modularity 0.13). Five VOSStudio runs with different seeds gave VOS quality
between 55,410 and 56,369, with VOSviewer's 55,936 inside that range and one run above it. Different runs of either
program give different partitions of this network. That makes it a property of the data, not a difference between the
tools. Use **Analyse → Cluster stability** to check this for your own maps: it re-runs clustering with different
seeds and reports how much the partitions agree.

**Speed.** VOSStudio's layout and clustering took 3 ms and 1 ms for kw, 79 ms and 30 ms for journal, and 2.4 s and
0.6 s for oa-coupling-docs (single run of the harness on a 2-core machine).

## What the validation changed

### 1. Clustering objective (algorithm change)

The first comparison matched on the sample networks but not on the journal network (ARI 0.574, 6 vs 7 clusters) or
on cite-docs (ARI 0.693). VOSStudio's partition had higher modularity, and VOSviewer's had higher VOS quality. The two
programs were optimising different functions:

- **VOSviewer** (for association strength and fractionalization) maximises the VOS clustering quality function of
  Waltman, van Eck & Noyons (2010):

  `V = Σ over pairs i<j in the same cluster of (s_ij − γ)`, with `s_ij = 2m · a_ij / (k_i · k_j)`

  In Leiden terms this is a constant Potts model on the normalised weights with unit node weights.
- **VOSStudio up to 1.5.2** maximised modularity on the same normalised weights (node weights equal to strengths,
  resolution scaled by 1/2m).

Both objectives often have the same optimum, which is why most sample networks matched. Since 1.6.0, VOSStudio's
Leiden optimises the VOS quality function, as VOSviewer does. The LinLog/modularity normalisation still uses
modularity, again as VOSviewer does. After the change, cite-docs and journal are identical (ARI 1.000).

Effect for users: clusterings of existing projects can change slightly when they are re-clustered in 1.6.0. The
"Modularity Q" shown in the app is still modularity, as a familiar summary measure, so it can be a little lower than
before for the same data. The Methods paragraph now names the objective.

### 2. Byte order mark in VOSviewer files (import bug)

VOSviewer writes map and network files with a UTF-8 byte order mark. VOSStudio 1.5.2 treated the mark as part of the
first column header. As a result the id column was not recognised and a phantom item was created. On the journal
example this produced a 233-item map instead of 232 and broke the id-based link matching. Since 1.6.0 the import
strips the mark.

## Reproducing

```
cd validation
g++ -std=c++17 -O1 -I../src/core vsval.cpp ../src/core/*.cpp -o vsval -pthread
./vsval export nets <files...>        # write <name>_in.txt and <name>_network.txt for a data set
./runall.sh vos                        # needs VOSviewer.jar in validation/vos and Java 8+
cat results2.txt
```

`compare.py A B` compares two VOSviewer map files (ARI, NMI, Procrustes, distance correlations). `vosq.py net map...`
computes the VOS quality of each map's partition.

## Not covered

- **Network construction** (counting, thresholds, term extraction): VOSviewer's command line cannot import
  bibliographic databases. Construction is covered by the project tests (every analysis type and unit on the sample
  data) and was cross-checked against the original VOSStudio web version, not against VOSviewer.
- **Bibliometrix / Biblioshiny** comparison of the descriptive tables is planned for a later release.
