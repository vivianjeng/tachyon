#ifndef TACHYON_CRYPTO_COMMITMENTS_POLYNOMIAL_OPENINGS_H_
#define TACHYON_CRYPTO_COMMITMENTS_POLYNOMIAL_OPENINGS_H_

#include <utility>
#include <vector>

#include "absl/container/btree_set.h"
#include "gtest/gtest_prod.h"

#include "tachyon/base/containers/container_util.h"
#include "tachyon/base/logging.h"
#include "tachyon/base/ref.h"
#include "tachyon/math/polynomials/univariate/lagrange_interpolation.h"

namespace tachyon::crypto {

// A single polynomial oracle with a single opening.
// The type of polynomial oracle can be either |Poly| in |CreateOpeningProof()|
// and |Commitment| in |VerifyOpeningProof()|.
template <typename Poly, typename PolyOracle = Poly>
struct PolynomialOpening {
  using Field = typename Poly::Field;
  using Point = typename Poly::Point;

  // polynomial Pᵢ or commitment Cᵢ
  base::DeepRef<const PolyOracle> poly_oracle;
  // xᵢ
  base::DeepRef<const Point> point;
  // Pᵢ(xᵢ)
  Field opening;

  PolynomialOpening() = default;
  PolynomialOpening(base::DeepRef<const PolyOracle> poly_oracle,
                    base::DeepRef<const Point> point, Field&& opening)
      : poly_oracle(poly_oracle), point(point), opening(std::move(opening)) {}
};

// A single polynomial oracle with multi openings.
template <typename Poly, typename PolyOracle = Poly>
struct PolynomialOpenings {
  using Field = typename Poly::Field;

  // polynomial Pᵢ or commitment Cᵢ
  base::DeepRef<const PolyOracle> poly_oracle;
  // [Pᵢ(x₀), Pᵢ(x₁), Pᵢ(x₂)]
  std::vector<Field> openings;

  PolynomialOpenings() = default;
  PolynomialOpenings(base::DeepRef<const PolyOracle> poly_oracle,
                     std::vector<Field>&& openings)
      : poly_oracle(poly_oracle), openings(std::move(openings)) {}
};

// Multi polynomial oracles with multi openings grouped by shared points.
template <typename Poly, typename PolyOracle = Poly>
struct GroupedPolynomialOpenings {
  using Field = typename Poly::Field;
  using Point = typename Poly::Point;

  // [{P₀, [P₀(x₀), P₀(x₁), P₀(x₂)]}, {P₁, [P₁(x₀), P₁(x₁), P₁(x₂)]}]
  std::vector<PolynomialOpenings<Poly, PolyOracle>> poly_openings_vec;
  // [x₀, x₁, x₂]
  std::vector<base::DeepRef<const Point>> point_refs;

  GroupedPolynomialOpenings() = default;
  GroupedPolynomialOpenings(
      std::vector<PolynomialOpenings<Poly, PolyOracle>>&& poly_openings_vec,
      std::vector<base::DeepRef<const Point>>&& point_refs)
      : poly_openings_vec(std::move(poly_openings_vec)),
        point_refs(std::move(point_refs)) {}

  // Create a low degree extension that is a linear combination with a set of
  // low degree extensions based on every |poly_openings_vec.openings| and
  // shared |points|.
  Poly CreateCombinedLowDegreeExtensions(
      const Field& r, std::vector<Poly>& low_degree_extensions) const {
    std::vector<Point> owned_points = CreateOwnedPoints();
    low_degree_extensions = CreateLowDegreeExtensions(owned_points);
    return CombineLowDegreeExtensions(r, owned_points, low_degree_extensions);
  }

 private:
  FRIEND_TEST(PolynomialOpeningsTest, CreateCombinedLowDegreeExtensions);

  // TODO(chokobole): Since |CreateLowDegreeExtensions()| and
  // |CombineLowDegreeExtensions()| internally access to |Point| with an
  // indexing operator, if we use a vector of |base::DeepRef<const Point>| it
  // can't access as we expect.
  std::vector<Point> CreateOwnedPoints() const {
    return base::Map(point_refs,
                     [](const base::DeepRef<const Point>& p) { return *p; });
  }

  // Create a set of low degree extensions based on every
  // |poly_openings.openings| and shared |points|.
  std::vector<Poly> CreateLowDegreeExtensions(
      const std::vector<Point>& owned_points) const {
    return base::Map(
        poly_openings_vec,
        [&owned_points](const PolynomialOpenings<Poly>& poly_openings) {
          Poly low_degree_extension;
          CHECK(math::LagrangeInterpolate(owned_points, poly_openings.openings,
                                          &low_degree_extension));
          return low_degree_extension;
        });
  }

  Poly CombineLowDegreeExtensions(
      const Field& r, const std::vector<Point>& owned_points,
      const std::vector<Poly>& low_degree_extensions) const {
    // numerators: [P₀(X) - R₀(X), P₁(X) - R₁(X), P₂(X) - R₂(X)]
    std::vector<Poly> numerators = base::Map(
        poly_openings_vec,
        [&low_degree_extensions](
            size_t i, const PolynomialOpenings<Poly>& poly_openings) {
          return *poly_openings.poly_oracle - low_degree_extensions[i];
        });

    // Combine numerator polynomials with powers of |r|.
    // N(X) = (P₀(X) - R₀(X)) + r(P₁(X) - R₁(X)) + r²(P₂(X) - R₂(X))
    Poly& n = Poly::LinearizeInPlace(numerators, r);

    // Divide combined polynomial by vanishing polynomial of evaluation points.
    // H(X) = N(X) / (X - x₀)(X - x₁)(X - x₂)
    Poly vanishing_poly = Poly::FromRoots(owned_points);
    return n /= vanishing_poly;
  }
};

template <typename Poly, typename PolyOracle = Poly>
class PolynomialOpeningGrouper {
 public:
  using Field = typename Poly::Field;
  using Point = typename Poly::Point;
  using PolyOracleDeepRef = base::DeepRef<const PolyOracle>;
  using PointDeepRef = base::DeepRef<const Point>;

  const std::vector<GroupedPolynomialOpenings<Poly, PolyOracle>>&
  grouped_poly_openings_vec() const {
    return grouped_poly_openings_vec_;
  }
  const absl::btree_set<PointDeepRef>& super_point_set() const {
    return super_point_set_;
  }

  void GroupByPolyAndPoints(
      const std::vector<PolynomialOpening<Poly, PolyOracle>>& poly_openings) {
    // Group |poly_openings| by polynomial.
    // poly_openings_grouped_by_poly[0]: {P₀, [x₀, x₁, x₂]}
    // poly_openings_grouped_by_poly[1]: {P₁, [x₀, x₁, x₂]}
    // poly_openings_grouped_by_poly[2]: {P₂, [x₀, x₁, x₂]}
    // poly_openings_grouped_by_poly[3]: {P₃, [x₂, x₃]}
    // poly_openings_grouped_by_poly[4]: {P₄, [x₄]}
    std::vector<PolyOracleGroupedPair> poly_openings_grouped_by_poly =
        GroupByPoly(poly_openings);

    // Group |poly_openings_grouped_by_poly| by points.
    // poly_openings_grouped_by_poly_and_points[0]: {[x₀, x₁, x₂], [P₀, P₁, P₂]}
    // poly_openings_grouped_by_poly_and_points[1]: {[x₂, x₃], [P₃]}
    // poly_openings_grouped_by_poly_and_points[2]: {[x₄], [P₄]}
    std::vector<PointGroupedPair> poly_openings_grouped_by_poly_and_points =
        GroupByPoints(poly_openings_grouped_by_poly);

    // Construct openings vectors from the |poly_openings_grouped_by_poly|.
    // Each contains oracles and the corresponding evaluation points.
    // grouped_poly_openings_vec_[0]: {[P₀, P₁, P₂], [x₀, x₁, x₂]}
    // grouped_poly_openings_vec_[1]: {[P₃], [x₂, x₃]}
    // grouped_poly_openings_vec_[2]: {[P₄], [x₄]}
    CreateMultiPolynomialOpenings(poly_openings,
                                  poly_openings_grouped_by_poly_and_points);
  }

 private:
  FRIEND_TEST(PolynomialOpeningsTest, GroupByPolyAndPoints);

  struct PolyOracleGroupedPair {
    PolyOracleDeepRef poly_oracle;
    absl::btree_set<PointDeepRef> points;
  };

  std::vector<PolyOracleGroupedPair> GroupByPoly(
      const std::vector<PolynomialOpening<Poly, PolyOracle>>& poly_openings) {
    std::vector<PolyOracleGroupedPair> ret;
    ret.reserve(poly_openings.size());
    for (const PolynomialOpening<Poly, PolyOracle>& poly_opening :
         poly_openings) {
      super_point_set_.insert(poly_opening.point);

      auto it = std::find_if(
          ret.begin(), ret.end(),
          [&poly_opening](
              const PolyOracleGroupedPair& poly_oracle_grouped_pair) {
            return poly_oracle_grouped_pair.poly_oracle ==
                   poly_opening.poly_oracle;
          });

      if (it != ret.end()) {
        it->points.insert(poly_opening.point);
      } else {
        PolyOracleGroupedPair new_pair;
        new_pair.poly_oracle = poly_opening.poly_oracle;
        new_pair.points.insert(poly_opening.point);
        ret.push_back(new_pair);
      }
    }
    return ret;
  }

  struct PointGroupedPair {
    absl::btree_set<PointDeepRef> points;
    std::vector<PolyOracleDeepRef> polys;
  };

  std::vector<PointGroupedPair> GroupByPoints(
      const std::vector<PolyOracleGroupedPair>& poly_openings_grouped_by_poly) {
    std::vector<PointGroupedPair> ret;
    ret.reserve(poly_openings_grouped_by_poly.size());
    for (const auto& poly_oracle_grouped_pair : poly_openings_grouped_by_poly) {
      auto it = std::find_if(ret.begin(), ret.end(),
                             [&poly_oracle_grouped_pair](
                                 const PointGroupedPair& point_grouped_pair) {
                               return point_grouped_pair.points ==
                                      poly_oracle_grouped_pair.points;
                             });

      if (it != ret.end()) {
        it->polys.push_back(poly_oracle_grouped_pair.poly_oracle);
      } else {
        PointGroupedPair new_pair;
        new_pair.points = poly_oracle_grouped_pair.points;
        new_pair.polys.push_back(poly_oracle_grouped_pair.poly_oracle);
        ret.push_back(new_pair);
      }
    }
    return ret;
  }

  void CreateMultiPolynomialOpenings(
      const std::vector<PolynomialOpening<Poly, PolyOracle>>& poly_openings,
      const std::vector<PointGroupedPair>&
          poly_openings_grouped_by_poly_and_points) {
    grouped_poly_openings_vec_.reserve(
        poly_openings_grouped_by_poly_and_points.size());

    for (const auto& [points, polys] :
         poly_openings_grouped_by_poly_and_points) {
      std::vector<PointDeepRef> points_vec(points.begin(), points.end());

      std::vector<PolynomialOpenings<Poly, PolyOracle>> poly_openings_vec =
          base::Map(polys, [&poly_openings,
                            &points_vec](PolyOracleDeepRef poly_oracle) {
            std::vector<Field> openings = base::Map(
                points_vec, [poly_oracle, &poly_openings](PointDeepRef point) {
                  return GetOpeningFromPolyOpenings(poly_openings, poly_oracle,
                                                    point);
                });
            return PolynomialOpenings<Poly, PolyOracle>(poly_oracle,
                                                        std::move(openings));
          });

      grouped_poly_openings_vec_.push_back(
          GroupedPolynomialOpenings<Poly, PolyOracle>(
              std::move(poly_openings_vec), std::move(points_vec)));
    }
  }

  static Field GetOpeningFromPolyOpenings(
      const std::vector<PolynomialOpening<Poly, PolyOracle>>& poly_openings,
      PolyOracleDeepRef poly_oracle, PointDeepRef point) {
    auto it = std::find_if(
        poly_openings.begin(), poly_openings.end(),
        [poly_oracle,
         point](const PolynomialOpening<Poly, PolyOracle>& poly_opening) {
          return poly_opening.poly_oracle == poly_oracle &&
                 poly_opening.point == point;
        });
    CHECK(it != poly_openings.end());
    return it->opening;
  }

  // |grouped_poly_openings_vec_| is a list of
  // |GroupedPolynomialOpenings|, which is the result of list of
  // |PolynomialOpening| grouped by poly_oracle and points.
  // {[P₀, P₁, P₂], [x₀, x₁, x₂]}
  // {[P₃], [x₂, x₃]}
  // {[P₄], [x₄]}
  std::vector<GroupedPolynomialOpenings<Poly, PolyOracle>>
      grouped_poly_openings_vec_;
  // |super_point_set_| is all the points that appear in opening.
  // [x₀, x₁, x₂, x₃, x₄]
  absl::btree_set<base::DeepRef<const Point>> super_point_set_;
};

}  // namespace tachyon::crypto

#endif  // TACHYON_CRYPTO_COMMITMENTS_POLYNOMIAL_OPENINGS_H_
