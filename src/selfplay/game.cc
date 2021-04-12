/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2021 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/

#include "selfplay/game.h"

#include <algorithm>

#include "mcts/stoppers/common.h"
#include "mcts/stoppers/factory.h"
#include "mcts/stoppers/stoppers.h"
#include "neural/writer.h"

namespace lczero {

namespace {
const OptionId kReuseTreeId{"reuse-tree", "ReuseTree",
                            "Reuse the search tree between moves."};
const OptionId kResignPercentageId{
    "resign-percentage", "ResignPercentage",
    "Resign when win percentage drops below specified value."};
const OptionId kResignWDLStyleId{
    "resign-wdlstyle", "ResignWDLStyle",
    "If set, resign percentage applies to any output state being above "
    "100% minus the percentage instead of winrate being below."};
const OptionId kResignEarliestMoveId{"resign-earliest-move",
                                     "ResignEarliestMove",
                                     "Earliest move that resign is allowed."};
const OptionId kMinimumAllowedVistsId{
    "minimum-allowed-visits", "MinimumAllowedVisits",
    "Unless the selected move is the best move, temperature based selection "
    "will be retried until visits of selected move is greater than or equal to "
    "this threshold."};
const OptionId kUciChess960{
    "chess960", "UCI_Chess960",
    "Castling moves are encoded as \"king takes rook\"."};
const OptionId kSyzygyTablebaseId{
    "syzygy-paths", "SyzygyPath",
    "List of Syzygy tablebase directories, list entries separated by system "
    "separator (\";\" for Windows, \":\" for Linux).",
    's'};

const OptionId kValueRepairThresholdId{
    "value-repair-threshold", "ValueRepairThreshold",
    "Reduce temperature to zero when the absolute difference between Q of best "
    "eval and Q of root is above this threshold. Since Q ranges from -1 to +1 "
    "this threshold is a no-op at the default value 2.0."};

}  // namespace

void SelfPlayGame::PopulateUciParams(OptionsParser* options) {
  options->Add<BoolOption>(kReuseTreeId) = false;
  options->Add<BoolOption>(kResignWDLStyleId) = false;
  options->Add<FloatOption>(kResignPercentageId, 0.0f, 100.0f) = 0.0f;
  options->Add<IntOption>(kResignEarliestMoveId, 0, 1000) = 0;
  options->Add<IntOption>(kMinimumAllowedVistsId, 0, 1000000) = 0;
  options->Add<BoolOption>(kUciChess960) = false;
  PopulateTimeManagementOptions(RunType::kSelfplay, options);
  options->Add<StringOption>(kSyzygyTablebaseId);
  options->Add<FloatOption>(kValueRepairThresholdId, 0.0f, 2.0f) = 2.0f;
}

SelfPlayGame::SelfPlayGame(PlayerOptions white, PlayerOptions black,
                           bool shared_tree, const Opening& opening)
    : options_{white, black},
      chess960_{white.uci_options->Get<bool>(kUciChess960) ||
                black.uci_options->Get<bool>(kUciChess960)} {
  orig_fen_ = opening.start_fen;
  tree_[0] = std::make_shared<NodeTree>();
  tree_[0]->ResetToPosition(orig_fen_, {});

  if (shared_tree) {
    tree_[1] = tree_[0];
  } else {
    tree_[1] = std::make_shared<NodeTree>();
    tree_[1]->ResetToPosition(orig_fen_, {});
  }
  for (Move m : opening.moves) {
    tree_[0]->MakeMove(m);
    if (tree_[0] != tree_[1]) tree_[1]->MakeMove(m);
  }
}

void SelfPlayGame::Play(int white_threads, int black_threads, bool training,
                        SyzygyTablebase* syzygy_tb, bool tb_adjudicate,
                        bool enable_resign) {
  bool blacks_move = tree_[0]->IsBlackToMove();

  // Value repair. This is supposed to be local per game.
  bool ignore_temp=false;

  // Do moves while not end of the game. (And while not abort_)
  while (!abort_) {
    game_result_ = tree_[0]->GetPositionHistory().ComputeGameResult();

    // Support adjudications START
    if (syzygy_tb != nullptr && tb_adjudicate &&
        game_result_ == GameResult::UNDECIDED) {
      auto board = tree_[0]->GetPositionHistory().Last().GetBoard();
      if (board.castlings().no_legal_castle() &&
          (board.ours() | board.theirs()).count() <=
              syzygy_tb->max_cardinality()) {
        auto tb_side_black = (tree_[0]->GetPlyCount() % 2) == 1;
        ProbeState state;
        const WDLScore wdl =
            syzygy_tb->probe_wdl(tree_[0]->GetPositionHistory().Last(), &state);
        // Only fail state means the WDL is wrong, probe_wdl may produce
        // correct result with a stat other than OK.
        if (state != FAIL) {
          if (wdl == WDL_WIN) {
            game_result_ =
                tb_side_black ? GameResult::BLACK_WON : GameResult::WHITE_WON;
          } else if (wdl == WDL_LOSS) {
            game_result_ =
                tb_side_black ? GameResult::WHITE_WON : GameResult::BLACK_WON;
          } else {  // Cursed wins and blessed losses count as draws.
            game_result_ = GameResult::DRAW;
          }
          adjudicated_ = true;
          break;
        }
      }
    }
    // Support adjudications STOP

    // If endgame, stop.
    if (game_result_ != GameResult::UNDECIDED) break;
    if (tree_[0]->GetPositionHistory().Last().GetGamePly() >= 450) {
      adjudicated_ = true;
      break;
    }
    // Initialize search.
    const int idx = blacks_move ? 1 : 0;
    if (!options_[idx].uci_options->Get<bool>(kReuseTreeId)) {
      tree_[idx]->TrimTreeAtHead();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (abort_) break;
      auto stoppers = options_[idx].search_limits.MakeSearchStopper();
      PopulateIntrinsicStoppers(stoppers.get(), options_[idx].uci_options);

      std::unique_ptr<UciResponder> responder =
          std::make_unique<CallbackUciResponder>(
              options_[idx].best_move_callback, options_[idx].info_callback);

      if (!chess960_) {
        // Remap FRC castling to legacy castling.
        responder = std::make_unique<Chess960Transformer>(
            std::move(responder), tree_[idx]->HeadPosition().GetBoard());
      }

      search_ = std::make_unique<Search>(
          *tree_[idx], options_[idx].network, std::move(responder),
          /* searchmoves */ MoveList(), std::chrono::steady_clock::now(),
          std::move(stoppers),
          /* infinite */ false, *options_[idx].uci_options, options_[idx].cache,
          syzygy_tb);
    }

    // Do search.
    search_->RunBlocking(blacks_move ? black_threads : white_threads);
    move_count_++;
    nodes_total_ += search_->GetTotalPlayouts();
    if (abort_) break;

    Move best_move;
    bool best_is_terminal;
    const auto best_eval = search_->GetBestEval(&best_move, &best_is_terminal);
    float eval = best_eval.wl;
    eval = (eval + 1) / 2;
    if (eval < min_eval_[idx]) min_eval_[idx] = eval;
    const int move_number = tree_[0]->GetPositionHistory().GetLength() / 2 + 1;
    auto best_w = (best_eval.wl + 1.0f - best_eval.d) / 2.0f;
    auto best_d = best_eval.d;
    auto best_l = best_w - best_eval.wl;
    max_eval_[0] = std::max(max_eval_[0], blacks_move ? best_l : best_w);
    max_eval_[1] = std::max(max_eval_[1], best_d);
    max_eval_[2] = std::max(max_eval_[2], blacks_move ? best_w : best_l);
    if (enable_resign &&
        move_number >=
            options_[idx].uci_options->Get<int>(kResignEarliestMoveId)) {
      const float resignpct =
          options_[idx].uci_options->Get<float>(kResignPercentageId) / 100;
      if (options_[idx].uci_options->Get<bool>(kResignWDLStyleId)) {
        auto threshold = 1.0f - resignpct;
        if (best_w > threshold) {
          game_result_ =
              blacks_move ? GameResult::BLACK_WON : GameResult::WHITE_WON;
          adjudicated_ = true;
          break;
        }
        if (best_l > threshold) {
          game_result_ =
              blacks_move ? GameResult::WHITE_WON : GameResult::BLACK_WON;
          adjudicated_ = true;
          break;
        }
        if (best_d > threshold) {
          game_result_ = GameResult::DRAW;
          adjudicated_ = true;
          break;
        }
      } else {
        if (eval < resignpct) {  // always false when resignpct == 0
          game_result_ =
              blacks_move ? GameResult::WHITE_WON : GameResult::BLACK_WON;
          adjudicated_ = true;
          break;
        }
      }
    }

    auto node = tree_[idx]->GetCurrentHead();
    Eval played_eval = best_eval;
    Move move;
    while (true) {
      move = search_->GetBestMove(ignore_temp).first;
      uint32_t max_n = 0;
      uint32_t cur_n = 0;

      for (auto& edge : node->Edges()) {
        if (edge.GetN() > max_n) {
          max_n = edge.GetN();
        }
        if (edge.GetMove(tree_[idx]->IsBlackToMove()) == move) {
          cur_n = edge.GetN();
          played_eval.wl = edge.GetWL(-node->GetWL());
          played_eval.d = edge.GetD(node->GetD());
          played_eval.ml = edge.GetM(node->GetM() - 1) + 1;
        }
      }
      // If 'best move' is less than allowed visits and not max visits,
      // discard it and try again.
      if (cur_n == max_n ||
          static_cast<int>(cur_n) >=
              options_[idx].uci_options->Get<int>(kMinimumAllowedVistsId)) {
        break;
      }
      PositionHistory history_copy = tree_[idx]->GetPositionHistory();
      Move move_for_history = move;
      if (tree_[idx]->IsBlackToMove()) {
        move_for_history.Mirror();
      }
      history_copy.Append(move_for_history);
      // Ensure not to discard games that are already decided.
      if (history_copy.ComputeGameResult() == GameResult::UNDECIDED) {
        auto move_list_to_discard = GetMoves();
        move_list_to_discard.push_back(move);
        options_[idx].discarded_callback({orig_fen_, move_list_to_discard});
      }
      search_->ResetBestMove();
    }

    if (training) {
      bool best_is_proof = best_is_terminal;  // But check for better moves.
      if (best_is_proof && best_eval.wl < 1) {
        auto best =
            (best_eval.wl == 0) ? GameResult::DRAW : GameResult::BLACK_WON;
        auto upper = best;
        for (const auto& edge : node->Edges()) {
          upper = std::max(edge.GetBounds().second, upper);
        }
        if (best < upper) {
          best_is_proof = false;
        }
      }
      // Append training data. The GameResult is later overwritten.
      const auto input_format =
          options_[idx].network->GetCapabilities().input_format;
      Eval orig_eval;
      NNCacheLock nneval =
          search_->GetCachedNNEval(tree_[idx]->GetCurrentHead());
      if (nneval) {
        orig_eval.wl = nneval->q;
        orig_eval.d = nneval->d;
        orig_eval.ml = nneval->m;
      } else {
        orig_eval.wl = std::numeric_limits<float>::quiet_NaN();
        orig_eval.d = std::numeric_limits<float>::quiet_NaN();
        orig_eval.ml = std::numeric_limits<float>::quiet_NaN();
      }
      training_data_.push_back(tree_[idx]->GetCurrentHead()->GetV6TrainingData(
          GameResult::UNDECIDED, tree_[idx]->GetPositionHistory(),
          search_->GetParams().GetHistoryFill(), input_format, best_eval,
          played_eval, orig_eval, best_is_proof, best_move, move));

      // Value repair

      // Unless we are not already in a game where this has triggered,
      // check if position should trigger temp=0.

      // This check depends on orig_eval.wl which is not available earlier,
      // but it this means that the move selection (which is earlier)
      // can have temperature. To fix that, move the whole block to
      // come before move selection, But I didn't know the code well
      // enough to judge if that would backfire.

      if (!ignore_temp) {
	float surprise=std::fabs(best_eval.wl - orig_eval.wl);
	if (surprise > options_[0].uci_options->Get<float>(
					  kValueRepairThresholdId)) {
	  ignore_temp=true;
	}
      }
    }
    // Must reset the search before mutating the tree.
    search_.reset();

    // Add best move to the tree.
    tree_[0]->MakeMove(move);
    if (tree_[0] != tree_[1]) tree_[1]->MakeMove(move);
    blacks_move = !blacks_move;
  }
}

std::vector<Move> SelfPlayGame::GetMoves() const {
  std::vector<Move> moves;
  for (Node* node = tree_[0]->GetCurrentHead();
       node != tree_[0]->GetGameBeginNode(); node = node->GetParent()) {
    moves.push_back(node->GetParent()->GetEdgeToNode(node)->GetMove());
  }
  std::vector<Move> result;
  Position pos = tree_[0]->GetPositionHistory().Starting();
  while (!moves.empty()) {
    Move move = moves.back();
    moves.pop_back();
    if (!chess960_) move = pos.GetBoard().GetLegacyMove(move);
    pos = Position(pos, move);
    // Position already flipped, therefore flip the move if white to move.
    if (!pos.IsBlackToMove()) move.Mirror();
    result.push_back(move);
  }
  return result;
}

float SelfPlayGame::GetWorstEvalForWinnerOrDraw() const {
  // TODO: This assumes both players have the same resign style.
  // Supporting otherwise involves mixing the meaning of worst.
  if (options_[0].uci_options->Get<bool>(kResignWDLStyleId)) {
    if (game_result_ == GameResult::WHITE_WON) {
      return std::max(max_eval_[1], max_eval_[2]);
    } else if (game_result_ == GameResult::BLACK_WON) {
      return std::max(max_eval_[1], max_eval_[0]);
    } else {
      return std::max(max_eval_[2], max_eval_[0]);
    }
  }
  if (game_result_ == GameResult::WHITE_WON) return min_eval_[0];
  if (game_result_ == GameResult::BLACK_WON) return min_eval_[1];
  return std::min(min_eval_[0], min_eval_[1]);
}

void SelfPlayGame::Abort() {
  std::lock_guard<std::mutex> lock(mutex_);
  abort_ = true;
  if (search_) search_->Abort();
}

void SelfPlayGame::WriteTrainingData(TrainingDataWriter* writer) const {
  if (training_data_.empty()) return;
  // Base estimate off of best_m.  If needed external processing can use a
  // different approach.
  float m_estimate = training_data_.back().best_m + training_data_.size() - 1;
  for (auto chunk : training_data_) {
    bool black_to_move = chunk.side_to_move_or_enpassant;
    if (IsCanonicalFormat(static_cast<pblczero::NetworkFormat::InputFormat>(
            chunk.input_format))) {
      black_to_move = (chunk.invariance_info & (1u << 7)) != 0;
    }
    if (game_result_ == GameResult::WHITE_WON) {
      chunk.result_q = black_to_move ? -1 : 1;
      chunk.result_d = 0;
    } else if (game_result_ == GameResult::BLACK_WON) {
      chunk.result_q = black_to_move ? 1 : -1;
      chunk.result_d = 0;
    } else {
      chunk.result_q = 0;
      chunk.result_d = 1;
    }
    if (adjudicated_) {
      chunk.invariance_info |= 1u << 5; // Game adjudicated.
    }
    if (adjudicated_ && game_result_ == GameResult::UNDECIDED) {
      chunk.invariance_info |= 1u << 4; // Max game length exceeded.
    }
    chunk.plies_left = m_estimate;
    m_estimate -= 1.0f;
    writer->WriteChunk(chunk);
  }
}

std::unique_ptr<ChainedSearchStopper> SelfPlayLimits::MakeSearchStopper()
    const {
  auto result = std::make_unique<ChainedSearchStopper>();

  // always set VisitsStopper to avoid exceeding the limit 4000000000, the
  // default value when visits = 0
  result->AddStopper(std::make_unique<VisitsStopper>(visits, false));
  if (playouts >= 0) {
    result->AddStopper(std::make_unique<PlayoutsStopper>(playouts, false));
  }
  if (movetime >= 0) {
    result->AddStopper(std::make_unique<TimeLimitStopper>(movetime));
  }
  return result;
}

}  // namespace lczero
