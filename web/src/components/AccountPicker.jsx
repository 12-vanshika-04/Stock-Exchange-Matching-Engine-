import { useState } from 'react';
import { User } from 'lucide-react';

// Development login: the gateway trusts whichever account number is typed here.
export default function AccountPicker({ account, onChange }) {
  const [draft, setDraft] = useState(String(account));
  const valid = /^\d{1,4}$/.test(draft);

  function submit(event) {
    event.preventDefault();
    if (valid) onChange(Number(draft));
  }

  return (
    <form className="account" onSubmit={submit}>
      <label htmlFor="account">
        <User size={16} aria-hidden="true" /> Account
      </label>
      <input
        id="account"
        inputMode="numeric"
        value={draft}
        onChange={(e) => setDraft(e.target.value)}
        aria-invalid={!valid}
      />
      <button type="submit" disabled={!valid || Number(draft) === account}>
        Switch
      </button>
    </form>
  );
}