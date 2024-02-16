// Copyright 2020-2022 The Electric Coin Company
// Copyright 2022 The Halo2 developers
// Use of this source code is governed by a MIT/Apache-2.0 style license that
// can be found in the LICENSE-MIT.halo2 and the LICENCE-APACHE.halo2
// file.

#ifndef TACHYON_ZK_LOOKUP_HALO2_PROVER_IMPL_H_
#define TACHYON_ZK_LOOKUP_HALO2_PROVER_IMPL_H_

#include <utility>
#include <vector>

#include "tachyon/base/containers/container_util.h"
#include "tachyon/base/ref.h"
#include "tachyon/zk/lookup/halo2/compress_expression.h"
#include "tachyon/zk/lookup/halo2/permute_expression_pair.h"
#include "tachyon/zk/lookup/halo2/prover.h"
#include "tachyon/zk/plonk/permutation/grand_product_argument.h"

namespace tachyon::zk::lookup::halo2 {

// static
template <typename Poly, typename Evals>
template <typename Domain>
LookupPair<Evals> Prover<Poly, Evals>::CompressPair(
    const Domain* domain, const LookupArgument<F>& argument, const F& theta,
    const SimpleEvaluator<Evals>& evaluator_tpl) {
  // A_compressed(X) = θᵐ⁻¹A₀(X) + θᵐ⁻²A₁(X) + ... + θAₘ₋₂(X) + Aₘ₋₁(X)
  Evals compressed_input = CompressExpressions(
      domain, argument.input_expressions(), theta, evaluator_tpl);

  // S_compressed(X) = θᵐ⁻¹S₀(X) + θᵐ⁻²S₁(X) + ... + θSₘ₋₂(X) + Sₘ₋₁(X)
  Evals compressed_table = CompressExpressions(
      domain, argument.table_expressions(), theta, evaluator_tpl);

  return {std::move(compressed_input), std::move(compressed_table)};
}

template <typename Poly, typename Evals>
template <typename Domain>
void Prover<Poly, Evals>::CompressPairs(
    const Domain* domain, const std::vector<LookupArgument<F>>& arguments,
    const F& theta, const SimpleEvaluator<Evals>& evaluator_tpl) {
  compressed_pairs_ = base::Map(
      arguments,
      [domain, &theta, &evaluator_tpl](const LookupArgument<F>& argument) {
        return CompressPair(domain, argument, theta, evaluator_tpl);
      });
}

// static
template <typename Poly, typename Evals>
template <typename Domain>
void Prover<Poly, Evals>::BatchCompressPairs(
    std::vector<Prover>& lookup_provers, const Domain* domain,
    const std::vector<LookupArgument<F>>& arguments, const F& theta,
    const std::vector<plonk::RefTable<Evals>>& tables,
    absl::Span<const F> challenges) {
  CHECK_EQ(lookup_provers.size(), tables.size());
  // NOTE(chokobole): It's safe to downcast because domain is already checked.
  int32_t n = static_cast<int32_t>(domain->size());
  for (size_t i = 0; i < lookup_provers.size(); ++i) {
    SimpleEvaluator<Evals> simple_evaluator(0, n, 1, tables[i], challenges);
    lookup_provers[i].CompressPairs(domain, arguments, theta, simple_evaluator);
  }
}

// static
template <typename Poly, typename Evals>
template <typename PCS>
LookupPair<BlindedPolynomial<Poly, Evals>> Prover<Poly, Evals>::PermutePair(
    ProverBase<PCS>* prover, const LookupPair<Evals>& compressed_pair) {
  // A'(X), S'(X)
  LookupPair<Evals> permuted_pair;
  CHECK(PermuteExpressionPair(prover, compressed_pair, &permuted_pair));

  F input_blind = prover->blinder().Generate();
  F table_blind = prover->blinder().Generate();
  return {{std::move(permuted_pair).TakeInput(), std::move(input_blind)},
          {std::move(permuted_pair).TakeTable(), std::move(table_blind)}};
}

template <typename Poly, typename Evals>
template <typename PCS>
void Prover<Poly, Evals>::PermutePairs(ProverBase<PCS>* prover) {
  permuted_pairs_ = base::Map(
      compressed_pairs_, [prover](const LookupPair<Evals>& compressed_pair) {
        return PermutePair(prover, compressed_pair);
      });
}

// static
template <typename Poly, typename Evals>
template <typename PCS>
void Prover<Poly, Evals>::BatchCommitPermutedPairs(
    const std::vector<Prover>& lookup_provers, ProverBase<PCS>* prover,
    size_t& commit_idx) {
  if (lookup_provers.empty()) return;

  if constexpr (PCS::kSupportsBatchMode) {
    for (const Prover& lookup_prover : lookup_provers) {
      for (const LookupPair<BlindedPolynomial<Poly, Evals>>& permuted_pair :
           lookup_prover.permuted_pairs_) {
        prover->BatchCommitAt(permuted_pair.input().evals(), commit_idx++);
        prover->BatchCommitAt(permuted_pair.table().evals(), commit_idx++);
      }
    }
  } else {
    for (const Prover& lookup_prover : lookup_provers) {
      for (const LookupPair<BlindedPolynomial<Poly, Evals>>& permuted_pair :
           lookup_prover.permuted_pairs_) {
        prover->CommitAndWriteToProof(permuted_pair.input().evals());
        prover->CommitAndWriteToProof(permuted_pair.table().evals());
      }
    }
  }
}

// static
template <typename Poly, typename Evals>
template <typename PCS>
BlindedPolynomial<Poly, Evals> Prover<Poly, Evals>::CreateGrandProductPoly(
    ProverBase<PCS>* prover, const LookupPair<Evals>& compressed_pair,
    const LookupPair<BlindedPolynomial<Poly, Evals>>& permuted_pair,
    const F& beta, const F& gamma) {
  return {plonk::GrandProductArgument::CreatePoly(
              prover, CreateNumeratorCallback(compressed_pair, beta, gamma),
              CreateDenominatorCallback(permuted_pair, beta, gamma)),
          prover->blinder().Generate()};
}

template <typename Poly, typename Evals>
template <typename PCS>
void Prover<Poly, Evals>::CreateGrandProductPolys(ProverBase<PCS>* prover,
                                                  const F& beta,
                                                  const F& gamma) {
  CHECK_EQ(compressed_pairs_.size(), permuted_pairs_.size());

  grand_product_polys_ =
      base::Map(compressed_pairs_,
                [this, prover, &beta, &gamma](
                    size_t i, const LookupPair<Evals>& compressed_pair) {
                  return CreateGrandProductPoly(
                      prover, compressed_pair, permuted_pairs_[i], beta, gamma);
                });
  compressed_pairs_.clear();
}

// static
template <typename Poly, typename Evals>
template <typename PCS>
void Prover<Poly, Evals>::BatchCommitGrandProductPolys(
    const std::vector<Prover>& lookup_provers, ProverBase<PCS>* prover,
    size_t& commit_idx) {
  if (lookup_provers.empty()) return;

  if constexpr (PCS::kSupportsBatchMode) {
    for (const Prover& lookup_prover : lookup_provers) {
      for (const BlindedPolynomial<Poly, Evals>& grand_product_poly :
           lookup_prover.grand_product_polys_) {
        prover->BatchCommitAt(grand_product_poly.evals(), commit_idx++);
      }
    }
  } else {
    for (const Prover& lookup_prover : lookup_provers) {
      for (const BlindedPolynomial<Poly, Evals>& grand_product_poly :
           lookup_prover.grand_product_polys_) {
        prover->CommitAndWriteToProof(grand_product_poly.evals());
      }
    }
  }
}

template <typename Poly, typename Evals>
template <typename Domain>
void Prover<Poly, Evals>::TransformEvalsToPoly(const Domain* domain) {
  for (LookupPair<BlindedPolynomial<Poly, Evals>>& permuted_pair :
       permuted_pairs_) {
    permuted_pair.input().TransformEvalsToPoly(domain);
    permuted_pair.table().TransformEvalsToPoly(domain);
  }
  for (BlindedPolynomial<Poly, Evals>& grand_product_poly :
       grand_product_polys_) {
    grand_product_poly.TransformEvalsToPoly(domain);
  }
}

template <typename Poly, typename Evals>
template <typename PCS>
void Prover<Poly, Evals>::Evaluate(ProverBase<PCS>* prover,
                                   const OpeningPointSet<F>& point_set) const {
  size_t size = grand_product_polys_.size();
  CHECK_EQ(size, permuted_pairs_.size());

#define EVALUATE(polynomial, point) \
  prover->EvaluateAndWriteToProof(polynomial.poly(), point_set.point)

  for (size_t i = 0; i < size; ++i) {
    const BlindedPolynomial<Poly, Evals>& grand_product_poly =
        grand_product_polys_[i];
    const LookupPair<BlindedPolynomial<Poly, Evals>>& permuted_pair =
        permuted_pairs_[i];

    EVALUATE(grand_product_poly, x);
    EVALUATE(grand_product_poly, x_next);
    EVALUATE(permuted_pair.input(), x);
    EVALUATE(permuted_pair.input(), x_prev);
    EVALUATE(permuted_pair.table(), x);
  }
#undef EVALUATE
}

template <typename Poly, typename Evals>
void Prover<Poly, Evals>::Open(
    const OpeningPointSet<F>& point_set,
    std::vector<crypto::PolynomialOpening<Poly>>& openings) const {
  size_t size = grand_product_polys_.size();
  CHECK_EQ(size, permuted_pairs_.size());

  base::DeepRef<const F> x_ref(&point_set.x);
  base::DeepRef<const F> x_prev_ref(&point_set.x_prev);
  base::DeepRef<const F> x_next_ref(&point_set.x_next);

#define OPENING(polynomial, point)                        \
  base::Ref<const Poly>(&polynomial.poly()), point##_ref, \
      polynomial.poly().Evaluate(point_set.point)

  for (size_t i = 0; i < size; ++i) {
    const BlindedPolynomial<Poly, Evals>& grand_product_poly =
        grand_product_polys_[i];
    const LookupPair<BlindedPolynomial<Poly, Evals>>& permuted_pair =
        permuted_pairs_[i];

    openings.emplace_back(OPENING(grand_product_poly, x));
    openings.emplace_back(OPENING(grand_product_poly, x_next));
    openings.emplace_back(OPENING(permuted_pair.input(), x));
    openings.emplace_back(OPENING(permuted_pair.input(), x_prev));
    openings.emplace_back(OPENING(permuted_pair.table(), x));
  }
#undef OPENING
}

// static
template <typename Poly, typename Evals>
base::ParallelizeCallback3<typename Poly::Field>
Prover<Poly, Evals>::CreateNumeratorCallback(
    const LookupPair<Evals>& compressed_pair, const F& beta, const F& gamma) {
  // (A_compressed(xᵢ) + β) * (S_compressed(xᵢ) + γ)
  return [&compressed_pair, &beta, &gamma](
             absl::Span<F> chunk, size_t chunk_index, size_t chunk_size) {
    size_t i = chunk_index * chunk_size;
    for (F& value : chunk) {
      value *= (compressed_pair.input()[i] + beta);
      value *= (compressed_pair.table()[i] + gamma);
      ++i;
    }
  };
}

// static
template <typename Poly, typename Evals>
base::ParallelizeCallback3<typename Poly::Field>
Prover<Poly, Evals>::CreateDenominatorCallback(
    const LookupPair<BlindedPolynomial<Poly, Evals>>& permuted_pair,
    const F& beta, const F& gamma) {
  // (A'(xᵢ) + β) * (S'(xᵢ) + γ)
  return [&permuted_pair, &beta, &gamma](
             absl::Span<F> chunk, size_t chunk_index, size_t chunk_size) {
    size_t i = chunk_index * chunk_size;
    for (F& value : chunk) {
      value = (permuted_pair.input().evals()[i] + beta) *
              (permuted_pair.table().evals()[i] + gamma);
      ++i;
    }
  };
}

}  // namespace tachyon::zk::lookup::halo2

#endif  // TACHYON_ZK_LOOKUP_HALO2_PROVER_IMPL_H_
