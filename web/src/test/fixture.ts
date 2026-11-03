import type { Console, Requirement } from "../shared/lib/console";

const req = (id: string, status: string, caught: boolean[], title = "A requirement"): Requirement => ({
  id,
  title,
  text: `${title}, stated plainly.`,
  method: "test",
  cites: [],
  status,
  evidence: "",
  links: [
    {
      test: `gtest:Suite.${id.replace("-", "_")}`,
      passed: true,
      kills: caught.some(Boolean) ? ["m0"] : [],
      proof: caught.some(Boolean) ? "proven" : "unproven",
    },
  ],
  mutants: caught.map((c, i) => ({
    name: `m${i}`,
    outcome: c ? "killed" : "survived",
    caught_by: c ? [`gtest:Suite.${id}`] : [],
    caught_by_unlinked: [],
  })),
});

export const fixture: Console = {
  generated_at: "2026-11-03T12:00:00+00:00",
  unit: { sw_version: "1.4.0", hw_id: "0x0F710001" },
  icds: [{ key: "imu", id: "IMU-SER", version: "1.2.0", title: "IMU", messages: 3 }],
  traceability: {
    summary: {
      requirements: 3,
      by_status: { verified: 2, weak: 1 },
      tests: 10,
      tests_passed: 10,
      links: 3,
      links_proven: 2,
      links_unproven: 1,
      links_system: 0,
      mutants: 4,
      mutants_caught_by_linked_tests: 3,
      gate: {
        all_requirements_verified: false,
        no_failing_tests: true,
        every_mutant_caught: false,
        no_claims_on_unknown_requirements: true,
        generated_files_current: true,
        pass: false,
      },
    },
    requirements: [
      req("SRS-001", "verified", [true]),
      req("SRS-011", "weak", [true, false], "Resynchronisation"),
      req("SRS-020", "verified", [true]),
    ],
    areas: [
      { name: "Interfaces", requirements: ["SRS-001"] },
      { name: "Serial links", requirements: ["SRS-011"] },
      { name: "IMU", requirements: ["SRS-020"] },
    ],
  },
  bench: { nominal: null, campaign: null },
  heldout: {},
  agents: null,
  assurance: {},
  sbom: [],
  release: { manifest: null, build_signature: null },
};
