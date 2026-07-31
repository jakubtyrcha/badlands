# Calibration report

Level 1. One row per pairing, sorted by how far apart the two threat targets are.

The design document states one invariant and asks one open question, and this table answers both empirically rather than by modelling them:

- **Invariant:** creatures of the same caliber should win about half their fights against each other. Read the top of the table, where the difference is 0.
- **Open question:** how does a difference in threat shift the expected win ratio? Read the rest of it.

| A | B | threat A | threat B | difference | A's win rate |
|---|---|---|---|---|---|
| Apprentice | Apprentice | 0.75 | 0.75 | +0.00 | 25% |
| Bandit | BanditArcher | 2.00 | 2.00 | +0.00 | 100% |
| Bandit | Bandit | 2.00 | 2.00 | +0.00 | 56% |
| BanditArcher | BanditArcher | 2.00 | 2.00 | +0.00 | 44% |
| BanditLeader | BanditLeader | 5.00 | 5.00 | +0.00 | 50% |
| Goblin | Goblin | 1.00 | 1.00 | +0.00 | 62% |
| GraveRobber | Goblin | 1.00 | 1.00 | +0.00 | 100% |
| GraveRobber | GraveRobber | 1.00 | 1.00 | +0.00 | 62% |
| Hunter | Hunter | 1.50 | 1.50 | +0.00 | 43% |
| Mercenary | Mercenary | 2.50 | 2.50 | +0.00 | 86% |
| MudGolem | MudGolem | 6.00 | 6.00 | +0.00 | 88% |
| Rat | Rat | 0.25 | 0.25 | +0.00 | 71% |
| Apprentice | Goblin | 0.75 | 1.00 | -0.25 | 83% |
| GraveRobber | Apprentice | 1.00 | 0.75 | +0.25 | 100% |
| Apprentice | Rat | 0.75 | 0.25 | +0.50 | 100% |
| Hunter | BanditArcher | 1.50 | 2.00 | -0.50 | 94% |
| Hunter | Bandit | 1.50 | 2.00 | -0.50 | 22% |
| Hunter | Goblin | 1.50 | 1.00 | +0.50 | 100% |
| Hunter | GraveRobber | 1.50 | 1.00 | +0.50 | 33% |
| Mercenary | BanditArcher | 2.50 | 2.00 | +0.50 | 100% |
| Mercenary | Bandit | 2.50 | 2.00 | +0.50 | 100% |
| GraveRobber | Rat | 1.00 | 0.25 | +0.75 | 100% |
| Hunter | Apprentice | 1.50 | 0.75 | +0.75 | 100% |
| Rat | Goblin | 0.25 | 1.00 | -0.75 | 0% |
| BanditLeader | MudGolem | 5.00 | 6.00 | -1.00 | 0% |
| Goblin | Bandit | 1.00 | 2.00 | -1.00 | 0% |
| Goblin | BanditArcher | 1.00 | 2.00 | -1.00 | 17% |
| GraveRobber | Bandit | 1.00 | 2.00 | -1.00 | 44% |
| GraveRobber | BanditArcher | 1.00 | 2.00 | -1.00 | 100% |
| Mercenary | Hunter | 2.50 | 1.50 | +1.00 | 100% |
| Apprentice | Bandit | 0.75 | 2.00 | -1.25 | 0% |
| Apprentice | BanditArcher | 0.75 | 2.00 | -1.25 | 6% |
| Hunter | Rat | 1.50 | 0.25 | +1.25 | 100% |
| Mercenary | GraveRobber | 2.50 | 1.00 | +1.50 | 100% |
| Mercenary | Goblin | 2.50 | 1.00 | +1.50 | 100% |
| Mercenary | Apprentice | 2.50 | 0.75 | +1.75 | 100% |
| Rat | Bandit | 0.25 | 2.00 | -1.75 | 0% |
| Rat | BanditArcher | 0.25 | 2.00 | -1.75 | 0% |
| Mercenary | Rat | 2.50 | 0.25 | +2.25 | 100% |
| Mercenary | BanditLeader | 2.50 | 5.00 | -2.50 | 0% |
| Bandit | BanditLeader | 2.00 | 5.00 | -3.00 | 0% |
| BanditArcher | BanditLeader | 2.00 | 5.00 | -3.00 | 0% |
| Hunter | BanditLeader | 1.50 | 5.00 | -3.50 | 0% |
| Mercenary | MudGolem | 2.50 | 6.00 | -3.50 | 0% |
| Bandit | MudGolem | 2.00 | 6.00 | -4.00 | 0% |
| BanditArcher | MudGolem | 2.00 | 6.00 | -4.00 | 0% |
| Goblin | BanditLeader | 1.00 | 5.00 | -4.00 | 0% |
| GraveRobber | BanditLeader | 1.00 | 5.00 | -4.00 | 0% |
| Apprentice | BanditLeader | 0.75 | 5.00 | -4.25 | 0% |
| Hunter | MudGolem | 1.50 | 6.00 | -4.50 | 0% |
| Rat | BanditLeader | 0.25 | 5.00 | -4.75 | 0% |
| Goblin | MudGolem | 1.00 | 6.00 | -5.00 | 0% |
| GraveRobber | MudGolem | 1.00 | 6.00 | -5.00 | 0% |
| Apprentice | MudGolem | 0.75 | 6.00 | -5.25 | 0% |
| Rat | MudGolem | 0.25 | 6.00 | -5.75 | 0% |

A pairing where every sample timed out is omitted: it has no opinion about who is stronger, and reporting 0% would be a lie about that.
