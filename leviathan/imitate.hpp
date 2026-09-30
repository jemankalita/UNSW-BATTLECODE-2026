#pragma once

// Move preferences learned from the top-10 teams' replays (scratchpad il/:
// features.js, train.js): a softmax over the four single-step moves, fitted on
// 130520 elite decisions, 64.9% held-out accuracy against
// 57.1% for "straight when legal". Features are what a
// dragon sees in its 7x7 window; each is standardized with the training mean
// and deviation before the weight applies. Order:
// pearlT, pearlGrad, pearlCount2, room, enemyHead1, enemyHead2, allyHead1, allyHead2, straight, enemyLongerAdj, enemyShorterAdj, edgeDist
namespace imitate {

static constexpr int COUNT = 12;
static constexpr double MEAN[COUNT] = {0.05155, 0.94912, 0.72020, 14.21875, 0.02334, 0.05481, 0.14788, 0.35418, 0.25000, 0.01723, 0.00611, 2.20185};
static constexpr double SD[COUNT] = {0.22112, 1.32007, 1.25844, 11.09222, 0.15616, 0.24770, 0.37677, 0.60535, 0.43301, 0.13370, 0.08030, 1.15310};
static constexpr double WEIGHT[COUNT] = {0.27800, 0.88700, -0.09200, 1.24200, -0.06000, 0.02000, -0.32100, -0.31000, 0.24100, -0.02600, -0.10100, 0.24900};

}  // namespace imitate
