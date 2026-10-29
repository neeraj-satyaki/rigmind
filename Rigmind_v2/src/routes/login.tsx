// ============= Full file contents =============

import { useEffect, useState, type FormEvent } from "react";
import { createFileRoute, useNavigate } from "@tanstack/react-router";
import { ShieldCheck } from "lucide-react";
import { toast } from "sonner";

import { Btn, Field, inputCls, pageHead } from "@/components/rig/ui";
import { SatyakiLogo } from "@/components/rig/AppShell";
import { NoteDialog, Prompt } from "@/components/rig/workflow";
import { ThemeToggle } from "@/components/rig/NavTools";
import RigMindMark from "@/components/loaders/RigMindMark";
import {
  DEFAULT_EMAIL,
  isValidEmail,
  saveProfileEmail,
  useProfileEmail,
} from "@/lib/profile";

export const Route = createFileRoute("/login")({
  head: pageHead("Sign in", "Sign in to your RigMind safety workspace."),
  component: Login,
});

function Login() {
  const nav = useNavigate();
  const go = () => nav({ to: "/fleet" });
  const saved = useProfileEmail();
  const [workEmail, setWorkEmail] = useState(saved);
  const [askOpen, setAskOpen] = useState(false);
  const [draft, setDraft] = useState("");

  useEffect(() => setWorkEmail(saved), [saved]);

  // Enterprise SSO now asks for the email first, so the address the person
  // types is the one carried into the workspace and remembered on this device.
  const openAsk = () => {
    setDraft(workEmail === DEFAULT_EMAIL ? "" : workEmail);
    setAskOpen(true);
  };
  const addEmail = async (e: FormEvent) => {
    e.preventDefault();
    const value = draft.trim();
    if (!isValidEmail(value)) {
      toast.error("Enter a valid work email address");
      return;
    }
    await saveProfileEmail(value);
    setWorkEmail(value);
    setAskOpen(false);
    toast.success(`${value} added on this device`);
  };

  return (
    <div className="flex min-h-screen flex-col">
      {/* One thin bar on every screen: the RigMind mark at the left, then the
          theme toggle and the Satyaki wordmark at the right. */}
      <header
        className="flex items-center gap-x-1.5 border-b border-border bg-card px-3 py-2"
        style={{ paddingTop: "calc(8px + env(safe-area-inset-top))" }}
      >
        {/* Static on the sign-in screen: the mark is branding here, not a loader. */}
        <RigMindMark size={30} animate={false} />

        <strong className="text-[16px] leading-none">RigMind</strong>

        <span className="ml-auto flex min-w-0 items-center gap-x-3 pl-1">
          <ThemeToggle />
          <SatyakiLogo />
        </span>
      </header>
      <main className="flex flex-1 items-center justify-center px-6 py-8">
        <form
          onSubmit={(e) => {
            e.preventDefault();
            go();
          }}
          className="w-full max-w-sm space-y-4 rounded-lg border border-border bg-card p-6"
        >
          <div>
            <h2 className="text-xl font-semibold">Welcome back</h2>
            <p className="text-muted-foreground">Sign in to your safety workspace.</p>
          </div>
          <Btn type="button" variant="primary" className="h-10 w-full" onClick={openAsk}>
            <ShieldCheck className="size-4" /> Continue with enterprise SSO
          </Btn>
          <div className="flex items-center gap-2 text-[11px] text-muted-foreground">
            <span className="h-px flex-1 bg-border" />
            or use an authorized local account
            <span className="h-px flex-1 bg-border" />
          </div>
          <Field label="Work email">
            <input
              className={inputCls}
              value={workEmail}
              onChange={(e) => setWorkEmail(e.target.value)}
            />
          </Field>
          <Field label="Password">
            <input type="password" className={inputCls} defaultValue="demopassword" />
          </Field>
          <div className="flex items-center justify-between text-[12px]">
            <label className="flex items-center gap-2">
              <input type="checkbox" defaultChecked className="accent-primary" /> Remember this
              device
            </label>
            <NoteDialog title="Request access" label="Need access?" text="Contact your platform administrator for production access. This demonstration account can enter using either sign-in button."/>
          </div>
          <Btn type="submit" variant="primary" className="h-10 w-full">
            Sign in to workspace
          </Btn>
          <p className="text-center text-[11px] text-muted-foreground">
            Your access is limited to assigned rigs and roles.
          </p>
        </form>
      </main>

      <Prompt
        title="Add your work email"
        description="Enter the email you want to use for this workspace. It is saved on this device only."
        open={askOpen}
        onClose={() => setAskOpen(false)}
      >
        <form onSubmit={addEmail} className="space-y-3 pt-2">
          <Field label="Work email">
            <input
              autoFocus
              type="email"
              className={inputCls}
              value={draft}
              onChange={(e) => setDraft(e.target.value)}
              placeholder="you@company.com"
            />
          </Field>
          <div className="flex justify-end gap-2">
            <Btn type="button" onClick={() => setAskOpen(false)}>
              Cancel
            </Btn>
            <Btn type="submit" variant="primary">
              Add email
            </Btn>
          </div>
        </form>
      </Prompt>
    </div>
  );
}
