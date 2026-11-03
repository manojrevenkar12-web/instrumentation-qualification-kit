import AxeBuilder from "@axe-core/playwright";
import { expect, test } from "@playwright/test";

test.describe("evidence console", () => {
  test("has no accessibility violations", async ({ page }) => {
    await page.goto("/");
    await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
    const r = await new AxeBuilder({ page }).analyze();
    expect(r.violations.map((v) => `${v.id}: ${v.nodes.length}`)).toEqual([]);
  });

  test("has no horizontal page scroll", async ({ page }) => {
    await page.goto("/");
    await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
    const over = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
    expect(over).toBeLessThanOrEqual(1);
  });

  test("a requirement can be chosen from the keyboard", async ({ page }) => {
    await page.goto("/");
    const tile = page.getByRole("button", { name: /^SRS-063/ });
    await tile.focus();
    await page.keyboard.press("Enter");
    await expect(tile).toHaveAttribute("aria-pressed", "true");
    await expect(page.getByRole("heading", { level: 3 })).toContainText("SRS-063");
  });

  test("explains a missing report and recovers on retry", async ({ page }) => {
    let fail = true;
    await page.route("**/data/console.json*", (route) => (fail ? route.fulfill({ status: 500, body: "" }) : route.continue()));
    await page.goto("/");
    await expect(page.getByRole("alert")).toContainText("HTTP 500");
    fail = false;
    await page.getByRole("button", { name: "Load again" }).click();
    await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
  });
});
