import { useState, type ReactNode } from "react";
import { Dialog, DialogContent, DialogTitle, DialogDescription } from "@/components/ui/dialog";
import { Btn, inputCls } from "./ui";

export function Check({
  children,
  checked,
  onChange,
}: {
  children: ReactNode;
  checked: boolean;
  onChange: (value: boolean) => void;
}) {
  return (
    <label className="flex items-center gap-2 py-2 text-[13px]">
      <input
        type="checkbox"
        className="size-4 accent-primary"
        checked={checked}
        onChange={(e) => onChange(e.target.checked)}
      />
      {children}
    </label>
  );
}
export function Prompt({
  title,
  description,
  children,
  open,
  onClose,
}: {
  title: string;
  description: string;
  children: ReactNode;
  open: boolean;
  onClose: () => void;
}) {
  return (
    <Dialog
      open={open}
      onOpenChange={(v) => {
        if (!v) onClose();
      }}
    >
      <DialogContent>
        <DialogTitle>{title}</DialogTitle>
        <DialogDescription>{description}</DialogDescription>
        {children}
      </DialogContent>
    </Dialog>
  );
}
export function NoteDialog({
  title,
  text,
  label = "View details",
  icon,
  className,
}: {
  title: string;
  text: string;
  label?: string;
  icon?: ReactNode;
  className?: string;
}) {
  const [open, setOpen] = useState(false);
  return (
    <>
      <Btn onClick={() => setOpen(true)} className={className}>
        {icon}
        {label}
      </Btn>
      <Prompt
        title={title}
        description="RigMind · Demonstration workspace"
        open={open}
        onClose={() => setOpen(false)}
      >
        <p className="whitespace-pre-line text-sm">{text}</p>
        <Btn onClick={() => setOpen(false)}>Close</Btn>
      </Prompt>
    </>
  );
}
export const textareaCls = `${inputCls} h-28 resize-y py-2`;
