# Duel matrix

Level 1, 9 staged separations per pairing. Each cell is the ROW creature's record against the column creature: `W-L-D`, then its win rate over decided duels, then the median duration in ticks.

Reported, not asserted. The threat column is the FIXED calibration target (game/src/threat_table.h) -- balancing moves the stats toward it, never it toward the stats.

The matrix is NOT symmetric by construction: `[row][col]` always stages the row creature on the left at -x and the column creature on the right, and the combat seed folds both slots, so the two directions are independent fights. Comparing a cell with its transpose is therefore a read on side bias, not a consistency check.

| vs | threat | Mercenary | Hunter | GraveRobber | Apprentice | Rat | Goblin | Bandit | BanditArcher | BanditLeader | MudGolem |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **Mercenary** | 2.5 | 6-1-2 86% <br><sub>538t</sub> | 9-0-0 100% <br><sub>458t</sub> | 9-0-0 100% <br><sub>758t</sub> | 9-0-0 100% <br><sub>372t</sub> | 9-0-0 100% <br><sub>79t</sub> | 9-0-0 100% <br><sub>204t</sub> | 9-0-0 100% <br><sub>452t</sub> | 9-0-0 100% <br><sub>358t</sub> | 0-9-0 0% <br><sub>327t</sub> | 0-9-0 0% <br><sub>420t</sub> |
| **Hunter** | 1.5 | 0-9-0 0% <br><sub>468t</sub> | 3-4-2 43% <br><sub>370t</sub> | 3-6-0 33% <br><sub>273t</sub> | 9-0-0 100% <br><sub>268t</sub> | 9-0-0 100% <br><sub>102t</sub> | 9-0-0 100% <br><sub>164t</sub> | 3-6-0 33% <br><sub>442t</sub> | 8-1-0 89% <br><sub>261t</sub> | 0-9-0 0% <br><sub>253t</sub> | 0-9-0 0% <br><sub>2597t</sub> |
| **GraveRobber** | 1 | 0-9-0 0% <br><sub>624t</sub> | 6-3-0 67% <br><sub>327t</sub> | 5-3-1 62% <br><sub>305t</sub> | 9-0-0 100% <br><sub>220t</sub> | 9-0-0 100% <br><sub>61t</sub> | 9-0-0 100% <br><sub>184t</sub> | 4-5-0 44% <br><sub>424t</sub> | 9-0-0 100% <br><sub>271t</sub> | 0-9-0 0% <br><sub>341t</sub> | 0-9-0 0% <br><sub>2609t</sub> |
| **Apprentice** | 0.75 | 0-9-0 0% <br><sub>322t</sub> | 0-9-0 0% <br><sub>323t</sub> | 0-9-0 0% <br><sub>232t</sub> | 2-6-1 25% <br><sub>296t</sub> | 9-0-0 100% <br><sub>154t</sub> | 8-1-0 89% <br><sub>453t</sub> | 0-9-0 0% <br><sub>533t</sub> | 1-8-0 11% <br><sub>276t</sub> | 0-9-0 0% <br><sub>235t</sub> | 0-9-0 0% <br><sub>2661t</sub> |
| **Rat** | 0.25 | 0-9-0 0% <br><sub>89t</sub> | 0-9-0 0% <br><sub>77t</sub> | 0-9-0 0% <br><sub>69t</sub> | 0-9-0 0% <br><sub>144t</sub> | 5-2-2 71% <br><sub>116t</sub> | 0-9-0 0% <br><sub>122t</sub> | 0-9-0 0% <br><sub>118t</sub> | 0-9-0 0% <br><sub>82t</sub> | 0-9-0 0% <br><sub>67t</sub> | 0-9-0 0% <br><sub>100t</sub> |
| **Goblin** | 1 | 0-9-0 0% <br><sub>184t</sub> | 0-9-0 0% <br><sub>225t</sub> | 0-9-0 0% <br><sub>203t</sub> | 2-7-0 22% <br><sub>367t</sub> | 9-0-0 100% <br><sub>88t</sub> | 5-3-1 62% <br><sub>263t</sub> | 0-9-0 0% <br><sub>221t</sub> | 0-9-0 0% <br><sub>331t</sub> | 0-9-0 0% <br><sub>150t</sub> | 0-9-0 0% <br><sub>255t</sub> |
| **Bandit** | 2 | 0-9-0 0% <br><sub>445t</sub> | 8-1-0 89% <br><sub>487t</sub> | 5-4-0 56% <br><sub>465t</sub> | 9-0-0 100% <br><sub>474t</sub> | 9-0-0 100% <br><sub>136t</sub> | 9-0-0 100% <br><sub>252t</sub> | 5-4-0 56% <br><sub>499t</sub> | 9-0-0 100% <br><sub>429t</sub> | 0-9-0 0% <br><sub>263t</sub> | 0-9-0 0% <br><sub>399t</sub> |
| **BanditArcher** | 2 | 0-9-0 0% <br><sub>325t</sub> | 0-9-0 0% <br><sub>297t</sub> | 0-9-0 0% <br><sub>298t</sub> | 9-0-0 100% <br><sub>311t</sub> | 9-0-0 100% <br><sub>66t</sub> | 6-3-0 67% <br><sub>324t</sub> | 0-9-0 0% <br><sub>367t</sub> | 4-5-0 44% <br><sub>352t</sub> | 0-9-0 0% <br><sub>240t</sub> | 0-9-0 0% <br><sub>1308t</sub> |
| **BanditLeader** | 5 | 9-0-0 100% <br><sub>327t</sub> | 9-0-0 100% <br><sub>307t</sub> | 9-0-0 100% <br><sub>314t</sub> | 9-0-0 100% <br><sub>271t</sub> | 9-0-0 100% <br><sub>75t</sub> | 9-0-0 100% <br><sub>156t</sub> | 9-0-0 100% <br><sub>242t</sub> | 9-0-0 100% <br><sub>240t</sub> | 4-4-1 50% <br><sub>644t</sub> | 0-9-0 0% <br><sub>610t</sub> |
| **MudGolem** | 6 | 9-0-0 100% <br><sub>436t</sub> | 9-0-0 100% <br><sub>2741t</sub> | 9-0-0 100% <br><sub>2701t</sub> | 9-0-0 100% <br><sub>2669t</sub> | 9-0-0 100% <br><sub>134t</sub> | 9-0-0 100% <br><sub>255t</sub> | 9-0-0 100% <br><sub>358t</sub> | 9-0-0 100% <br><sub>659t</sub> | 9-0-0 100% <br><sub>638t</sub> | 7-1-1 88% <br><sub>1229t</sub> |

A `-` win rate means every sample of that pairing timed out.
