#ifndef VENDORS_CIRCOM_CIRCOMLIB_R1CS_R1CS_H_
#define VENDORS_CIRCOM_CIRCOMLIB_R1CS_R1CS_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "circomlib/base/prime_field.h"
#include "circomlib/base/sections.h"
#include "circomlib/r1cs/constraint.h"
#include "tachyon/base/buffer/endian_auto_reset.h"
#include "tachyon/base/logging.h"
#include "tachyon/base/strings/string_util.h"

namespace tachyon::circom {
namespace v1 {

struct R1CS;

}  // namespace v1

struct R1CS {
  virtual ~R1CS() = default;

  virtual uint32_t GetVersion() const = 0;

  virtual v1::R1CS* ToV1() { return nullptr; }

  virtual bool Read(const base::ReadOnlyBuffer& buffer) = 0;

  virtual size_t GetNumInstanceVariables() const = 0;
  virtual size_t GetNumVariables() const = 0;
  virtual const std::vector<Constraint>& GetConstraints() const = 0;
  virtual const std::vector<uint64_t>& GetWireId2LabelIdMap() const = 0;
};

constexpr char kR1CSMagic[4] = {'r', '1', 'c', 's'};

namespace v1 {

enum class R1CSSectionType : uint32_t {
  kHeader = 0x1,
  kConstraints = 0x2,
  kWire2LabelIdMap = 0x3,
  kCustomGatesList = 0x4,
  kCustomGatesApplication = 0x5,
};

std::string_view R1CSSectionTypeToString(R1CSSectionType type);

struct R1CSHeaderSection {
  PrimeField modulus;
  // Total number of wires including ONE signal (Index 0).
  uint32_t num_wires;
  // Total number of public output wires. They should be starting at
  // idx 1.
  uint32_t num_public_outputs;
  // Total number of public input wires. They should be starting just
  // after the public output.
  uint32_t num_public_inputs;
  // Total number of private input wires. They should be starting just
  // after the public inputs.
  uint32_t num_private_inputs;
  // Total number of labels.
  uint64_t num_labels;
  // Total number of constraints.
  uint32_t num_constraints;

  bool operator==(const R1CSHeaderSection& other) const {
    return modulus == other.modulus && num_wires == other.num_wires &&
           num_public_outputs == other.num_public_outputs &&
           num_public_inputs == other.num_public_inputs &&
           num_private_inputs == other.num_private_inputs &&
           num_labels == other.num_labels &&
           num_constraints == other.num_constraints;
  }
  bool operator!=(const R1CSHeaderSection& other) const {
    return !operator==(other);
  }

  bool Read(const base::ReadOnlyBuffer& buffer) {
    base::EndianAutoReset reset(buffer, base::Endian::kLittle);
    if (!modulus.Read(buffer)) return false;
    return buffer.ReadMany(&num_wires, &num_public_outputs, &num_public_inputs,
                           &num_private_inputs, &num_labels, &num_constraints);
  }

  std::string ToString() const {
    return absl::Substitute(
        "{modulus: $0, num_wires: $1, num_public_outputs: $2, "
        "num_public_inputs: $3, num_private_inputs: $4 num_labels: $5, "
        "num_constraints: $6}",
        modulus.ToString(), num_wires, num_public_outputs, num_public_inputs,
        num_private_inputs, num_labels, num_constraints);
  }
};

struct R1CSConstraintsSection {
  std::vector<Constraint> constraints;

  bool operator==(const R1CSConstraintsSection& other) const {
    return constraints == other.constraints;
  }
  bool operator!=(const R1CSConstraintsSection& other) const {
    return constraints != other.constraints;
  }

  bool Read(const base::ReadOnlyBuffer& buffer,
            const R1CSHeaderSection& header) {
    base::EndianAutoReset reset(buffer, base::Endian::kLittle);
    uint32_t field_size = header.modulus.bytes.size();
    constraints.reserve(header.num_constraints);
    for (uint32_t i = 0; i < header.num_constraints; ++i) {
      Constraint constraint;
      for (uint32_t j = 0; j < 3; ++j) {
        uint32_t n;
        if (!buffer.Read(&n)) return false;
        std::vector<Term> terms(n);
        for (uint32_t k = 0; k < n; ++k) {
          if (!buffer.Read(&terms[k].wire_id)) return false;
          if (!terms[k].coefficient.Read(buffer, field_size)) return false;
        }
        if (j == 0) {
          constraint.a = {std::move(terms)};
        } else if (j == 1) {
          constraint.b = {std::move(terms)};
        } else {
          constraint.c = {std::move(terms)};
        }
      }
      constraints.push_back(std::move(constraint));
    }
    return true;
  }

  std::string ToString() const { return base::ContainerToString(constraints); }
};

struct R1CSWireId2LabelIdMapSection {
  std::vector<uint64_t> label_ids;

  bool operator==(const R1CSWireId2LabelIdMapSection& other) const {
    return label_ids == other.label_ids;
  }
  bool operator!=(const R1CSWireId2LabelIdMapSection& other) const {
    return label_ids != other.label_ids;
  }

  bool Read(const base::ReadOnlyBuffer& buffer,
            const R1CSHeaderSection& header) {
    base::EndianAutoReset reset(buffer, base::Endian::kLittle);
    label_ids.resize(header.num_wires);
    for (uint32_t i = 0; i < header.num_wires; ++i) {
      if (!buffer.Read(&label_ids[i])) return false;
    }
    return true;
  }

  std::string ToString() const { return base::ContainerToString(label_ids); }
};

struct R1CS : public circom::R1CS {
  R1CSHeaderSection header;
  R1CSConstraintsSection constraints;
  R1CSWireId2LabelIdMapSection wire_id_to_label_id_map;

  // circom::R1CS methods
  uint32_t GetVersion() const override { return 1; }
  R1CS* ToV1() override { return this; }

  bool Read(const base::ReadOnlyBuffer& buffer) override {
    Sections<R1CSSectionType> sections(buffer, &R1CSSectionTypeToString);
    if (!sections.Read()) return false;

    if (!sections.MoveTo(R1CSSectionType::kHeader)) return false;
    if (!header.Read(buffer)) return false;

    if (!sections.MoveTo(R1CSSectionType::kConstraints)) return false;
    if (!constraints.Read(buffer, header)) return false;

    if (!sections.MoveTo(R1CSSectionType::kWire2LabelIdMap)) return false;
    if (!wire_id_to_label_id_map.Read(buffer, header)) return false;
    return true;
  }

  size_t GetNumInstanceVariables() const override {
    return 1 + header.num_public_outputs + header.num_public_inputs;
  }

  size_t GetNumVariables() const override { return header.num_wires; }

  const std::vector<Constraint>& GetConstraints() const override {
    return constraints.constraints;
  }

  const std::vector<uint64_t>& GetWireId2LabelIdMap() const override {
    return wire_id_to_label_id_map.label_ids;
  }

  std::string ToString() const {
    return absl::Substitute(
        "{header: $0, constraints: $1, wire_id_to_label_id_map: $2}",
        header.ToString(), constraints.ToString(),
        wire_id_to_label_id_map.ToString());
  }
};

}  // namespace v1
}  // namespace tachyon::circom

#endif  // VENDORS_CIRCOM_CIRCOMLIB_R1CS_R1CS_H_
