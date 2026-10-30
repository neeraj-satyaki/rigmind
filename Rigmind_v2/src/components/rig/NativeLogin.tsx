import { useEffect, useState, type FormEvent } from "react";
import { Btn, Field, inputCls } from "@/components/rig/ui";
import { PasswordInput } from "@/components/rig/PasswordInput";
import { authStatus, createFirstAccount, signIn } from "@/lib/auth";

/**
 * Sign-in inside the desktop app. The first start asks to create the administrator account; every later start checks the
 * username and password with the host, which locks a username for a while after repeated failures.
 */
export function NativeLogin({ onDone }: { onDone: () => void }) {
  const [mode, setMode] = useState<"loading" | "setup" | "signin" | "unavailable">("loading");
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [confirm, setConfirm] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    authStatus()
      .then((s) => setMode(s.hasUsers ? "signin" : "setup"))
      .catch((e) => {
        setError(String(e instanceof Error ? e.message : e));
        setMode("unavailable");
      });
  }, []);

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    setError("");
    if (!username.trim() || !password) {
      setError("Enter the username and the password.");
      return;
    }
    if (mode === "setup" && password !== confirm) {
      setError("The two passwords are not the same.");
      return;
    }
    setBusy(true);
    try {
      if (mode === "setup") await createFirstAccount(username, password);
      else await signIn(username, password);
      onDone();
    } catch (err) {
      setPassword("");
      setConfirm("");
      setError(String(err instanceof Error ? err.message : err).replace(/^Error:\s*/, ""));
    } finally {
      setBusy(false);
    }
  };

  if (mode === "loading") return <p className="text-sm text-muted-foreground">Starting…</p>;
  if (mode === "unavailable")
    return (
      <div className="w-full max-w-sm space-y-3 rounded-lg border border-border bg-card p-6" role="alert">
        <h2 className="text-xl font-semibold">Sign-in is not available</h2>
        <p className="text-sm text-muted-foreground">The RigMind service did not answer. Close the app and start it again.</p>
        {error && <p className="font-mono text-xs text-muted-foreground">{error}</p>}
      </div>
    );

  const setup = mode === "setup";
  return (
    <form onSubmit={submit} className="w-full max-w-sm space-y-4 rounded-lg border border-border bg-card p-6" noValidate>
      <div>
        <h2 className="text-xl font-semibold">{setup ? "Create the first account" : "Welcome back"}</h2>
        <p className="text-muted-foreground">
          {setup ? "This account administers RigMind on this PC. It is kept on this device only." : "Sign in to your safety workspace."}
        </p>
      </div>
      <Field label="Username">
        <input
          className={inputCls}
          autoComplete="username"
          autoFocus
          value={username}
          onChange={(e) => setUsername(e.target.value)}
          aria-label="Username"
        />
      </Field>
      <Field label="Password">
        <PasswordInput autoComplete={setup ? "new-password" : "current-password"} value={password} onChange={(e) => setPassword(e.target.value)} aria-label="Password" />
      </Field>
      {setup && (
        <>
          <Field label="Repeat the password">
            <PasswordInput autoComplete="new-password" value={confirm} onChange={(e) => setConfirm(e.target.value)} aria-label="Repeat the password" />
          </Field>
          <p className="text-[11px] text-muted-foreground">At least 10 characters, with letters and a digit or symbol. Do not reuse the username.</p>
        </>
      )}
      {error && (
        <p role="alert" className="rounded-md border border-critical/40 bg-critical/10 px-3 py-2 text-sm text-critical">
          {error}
        </p>
      )}
      <Btn type="submit" variant="primary" className="h-10 w-full" disabled={busy}>
        {busy ? "Checking…" : setup ? "Create account and sign in" : "Sign in to workspace"}
      </Btn>
      <p className="text-center text-[11px] text-muted-foreground">Your access is limited to assigned rigs and roles.</p>
    </form>
  );
}
