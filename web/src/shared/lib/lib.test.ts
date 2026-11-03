import { describe, expect, it } from "vitest";
import { isConsole } from "./console";
import { pct, statusTone, testName, us } from "./status";

describe("status vocabulary", () => {
  it("maps statuses to tones", () => {
    expect(statusTone("verified")).toBe("proven");
    expect(statusTone("verified, system level (no mutants)")).toBe("system");
    expect(statusTone("weak")).toBe("weak");
    expect(statusTone("failing")).toBe("failed");
  });
  it("names tests for people", () => {
    expect(testName("gtest:Framing.CrcErrorsAreCounted")).toBe("Framing.CrcErrorsAreCounted");
    expect(testName("pytest:tests/system/test_bench.py::test_the_fault_campaign")).toBe("test_bench.py test_the_fault_campaign");
  });
  it("formats times and shares", () => {
    expect(us(349.2)).toBe("349 µs");
    expect(us(1450)).toBe("1.45 ms");
    expect(us(null)).toBe("–");
    expect(pct(1, 3)).toBe("33%");
    expect(pct(1, 0)).toBe("–");
  });
  it("recognises an evidence report", () => {
    expect(isConsole({ generated_at: "x", traceability: {} })).toBe(true);
    expect(isConsole({ generated_at: "x" })).toBe(false);
    expect(isConsole(null)).toBe(false);
  });
});
