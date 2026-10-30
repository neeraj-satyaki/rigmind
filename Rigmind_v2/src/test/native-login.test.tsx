import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";

const h = vi.hoisted(() => ({
  hasUsers: false,
  calls: [] as { method: string; params: Record<string, unknown> }[],
  fail: "" as string,
}));
vi.mock("@/lib/native-bridge", () => ({
  isNative: () => true,
  onNativeEvent: () => () => {},
  nativeCall: async (method: string, params: Record<string, unknown>) => {
    h.calls.push({ method, params });
    if (method === "auth.status") return { hasUsers: h.hasUsers };
    if (method === "auth.login" || method === "auth.setup") {
      if (h.fail) throw new Error(h.fail);
      return { username: String(params["username"]).trim().toLowerCase(), displayName: String(params["username"]).trim().toLowerCase(), role: "Administrator" };
    }
    return { ok: true };
  },
}));

import { NativeLogin } from "@/components/rig/NativeLogin";
import { currentOperator, getSession, signOut } from "@/lib/auth";

const type = (label: string, value: string) => fireEvent.change(screen.getByLabelText(label), { target: { value } });
const submit = async () => {
  await act(async () => {
    fireEvent.click(screen.getByRole("button", { name: /sign in to workspace|create account and sign in/i }));
  });
};

describe("sign-in inside the desktop app", () => {
  beforeEach(() => {
    h.calls.length = 0;
    h.fail = "";
    signOut();
    h.calls.length = 0;
  });

  it("first start: asks for the first account, checks the two passwords match, then creates it and signs in", async () => {
    h.hasUsers = false;
    const done = vi.fn();
    render(<NativeLogin onDone={done} />);
    await screen.findByText("Create the first account");
    type("Username", "Alice");
    type("Password", "correct horse 7");
    type("Repeat the password", "something else 1");
    await submit();
    expect(screen.getByRole("alert").textContent).toContain("not the same");
    expect(h.calls.some((c) => c.method === "auth.setup")).toBe(false); // nothing sent to the host
    type("Password", "correct horse 7");
    type("Repeat the password", "correct horse 7");
    await submit();
    await waitFor(() => expect(done).toHaveBeenCalled());
    expect(h.calls.find((c) => c.method === "auth.setup")?.params).toEqual({ username: "Alice", password: "correct horse 7" });
    expect(getSession()?.username).toBe("alice");
    expect(currentOperator().name).toBe("alice");
  });

  it("later starts: a wrong password shows the host's message, clears the field and does not sign in", async () => {
    h.hasUsers = true;
    h.fail = "Wrong username or password.";
    const done = vi.fn();
    render(<NativeLogin onDone={done} />);
    await screen.findByText("Welcome back");
    type("Username", "alice");
    type("Password", "not the password");
    await submit();
    expect(screen.getByRole("alert").textContent).toBe("Wrong username or password.");
    expect((screen.getByLabelText("Password") as HTMLInputElement).value).toBe(""); // cleared
    expect(done).not.toHaveBeenCalled();
    expect(getSession()).toBeNull();
    expect(h.calls.some((c) => c.method === "auth.login")).toBe(true);
  });

  it("shows the lockout message from the host", async () => {
    h.hasUsers = true;
    h.fail = "Too many attempts. Try again in 30 seconds.";
    render(<NativeLogin onDone={() => {}} />);
    await screen.findByText("Welcome back");
    type("Username", "alice");
    type("Password", "whatever-1234");
    await submit();
    expect(screen.getByRole("alert").textContent).toContain("Try again in 30 seconds");
  });

  it("right credentials sign in and remember who", async () => {
    h.hasUsers = true;
    const done = vi.fn();
    render(<NativeLogin onDone={done} />);
    await screen.findByText("Welcome back");
    type("Username", " Alice ");
    type("Password", "correct horse 7");
    await submit();
    await waitFor(() => expect(done).toHaveBeenCalled());
    expect(getSession()).toMatchObject({ username: "alice", role: "Administrator" });
    signOut();
    expect(getSession()).toBeNull();
    expect(h.calls.some((c) => c.method === "auth.logout")).toBe(true); // the host's audit trail hears about it
  });

  it("empty fields never reach the host", async () => {
    h.hasUsers = true;
    render(<NativeLogin onDone={() => {}} />);
    await screen.findByText("Welcome back");
    await submit();
    expect(screen.getByRole("alert").textContent).toContain("Enter the username and the password");
    expect(h.calls.some((c) => c.method === "auth.login")).toBe(false);
  });
});
