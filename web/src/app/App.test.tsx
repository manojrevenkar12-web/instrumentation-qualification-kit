import { act, cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { fixture } from "../test/fixture";
import { App } from "./App";

function respond(status: number, body: unknown) {
  vi.stubGlobal(
    "fetch",
    vi.fn(async () => new Response(typeof body === "string" ? body : JSON.stringify(body), { status })),
  );
}

afterEach(() => {
  cleanup();
  vi.unstubAllGlobals();
});

describe("evidence console", () => {
  it("shows a skeleton while loading", () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(() => new Promise(() => {})),
    );
    render(<App />);
    expect(screen.getByRole("status").textContent).toContain("Loading the evidence");
  });

  it("explains a missing report", async () => {
    respond(404, "");
    render(<App />);
    expect(await screen.findByText("No evidence has been generated for this build")).toBeTruthy();
  });

  it("reports a broken file and loads again on retry", async () => {
    respond(200, "{not json");
    render(<App />);
    expect(await screen.findByText("The report is not valid JSON.")).toBeTruthy();
    respond(200, fixture);
    await act(async () => {
      fireEvent.click(screen.getByRole("button", { name: "Load again" }));
    });
    expect(await screen.findByRole("heading", { level: 1 })).toBeTruthy();
  });

  it("refuses a file that is not an evidence report", async () => {
    respond(200, { hello: "world" });
    render(<App />);
    expect(await screen.findByText(/not an evidence report/)).toBeTruthy();
  });

  it("leads with the verdict and opens the first unverified requirement", async () => {
    respond(200, fixture);
    render(<App />);
    const h1 = await screen.findByRole("heading", { level: 1 });
    expect(h1.textContent).toContain("cannot ship yet: 1 of 3");
    expect(screen.getByRole("heading", { level: 3 }).textContent).toContain("SRS-011");
    expect(screen.getByText("not caught (survived)")).toBeTruthy();
  });

  it("selects a requirement from the strip", async () => {
    respond(200, fixture);
    render(<App />);
    const tile = await screen.findByRole("button", { name: /^SRS-020/ });
    fireEvent.click(tile);
    expect(tile.getAttribute("aria-pressed")).toBe("true");
    expect(await screen.findByRole("heading", { level: 3, name: /SRS-020/ })).toBeTruthy();
  });

  it("shows empty states for evidence that has not been produced", async () => {
    respond(200, fixture);
    render(<App />);
    expect(await screen.findByText("No bench runs yet")).toBeTruthy();
    expect(screen.getByText("No held-out set has run")).toBeTruthy();
    expect(screen.getByText("No agent drafts measured yet")).toBeTruthy();
  });
});
