# Running this in public: the operator checklist

**Not legal advice. Have a lawyer review the legal pages and this plan before a public launch.** This file and the
pages under `runtime/web/legal/` were written by a volunteer who is not a lawyer. They are a sensible starting
point, not a guarantee, and laws differ by place.

The design goal is to hold as little personal information as possible: sign-in asks only for the `openid` scope,
the server stores a keyed hash of the identity (not an email, name or profile), there are no cookies, analytics or
ads, and the only record that holds an IP address is sealed. The age notice is a self-declaration, not
verification (docs/web.md, "Legal pages and the age notice").

## 1. Fill in `runtime/web/site.json`

The repo ships it empty on purpose. Set: `siteName`, `operatorName`, `contactEmail`, `privacyEmail` (optional),
`dmcaAgent` (`name`, `address`, `email`, `phone`), `governingLaw` (a US state or a country), `serverLocation`
(for example "United States (Azure)"), `effectiveDate`, `termsVersion` and `minimumAge`. Optional: `auditRetentionDays`
(matches the server setting below) and `authServer`. Do not commit your real values to the public repo; keep them
in your deployment. An unfilled field shows a bright placeholder and a banner on the legal pages, so a
half-configured site is obvious.

Raise `termsVersion` whenever the terms or privacy notice change in a way that matters. Every visitor is then asked
again to confirm their age and accept.

## 2. Register a DMCA designated agent

The safe harbour of 17 U.S.C. section 512(c) (protection from liability for what users post) is **not available**
unless you have registered a designated agent and posted the agent's details.

- Register at https://dmca.copyright.gov: an online form and a small fee ($6 at the time of writing; check the
  current fee).
- It must be renewed every 3 years; the system emails reminders.
- The agent can be you.
- Consider a contact address that is not your home. Check whether a P.O. box is accepted (37 C.F.R. section 201.38)
  before relying on one.
- Put the same details in `dmcaAgent` in `site.json`.

## 3. Operate it safely

- Use a dedicated contact mailbox (for `contactEmail`, `privacyEmail` and the agent email), watched regularly.
- Run `innkeeperd` in its public, privacy-safe mode (`--public`), with `--key-vault` pointing at the server vault
  (the audit public key and its key id come from there) and `--audit-retention-days` (default 90). See the server
  repository for the details.
- Back up the database to Azure Blob Storage, and let a lifecycle-management rule delete each backup after **30
  days**. The privacy notice promises that erased data leaves backups within about that time. Long retention
  (complaints for a year, anything under a legal hold) happens in the live database, not in old backups, so no
  backup needs to outlive 30 days and the archive tier is not needed. Archive also charges for deletion before
  180 days.
  - If a preservation request ever needs a specific backup kept, put a legal hold (blob immutability) on that one
    blob rather than lengthening the rule.
  - The default server-side encryption covers the data at rest. Backups contain mail and posts in the clear, so
    lock the storage account down without private endpoints:
    - Turn off anonymous blob access and shared-key access, so the only way in is Entra ID.
    - The VM's managed identity (or the service principal) gets a role scoped to the backup container that can
      write and read but not delete. The lifecycle rule does the deleting.
    - Require TLS 1.2, and turn on blob soft delete and versioning.
    - To limit the network as well, add a free Microsoft.Storage service endpoint on the VM's subnet plus a
      virtual-network rule on the account. The "trusted Microsoft services" exception does not cover VMs.
      An IP rule for the VM's public address does not work from the same region, because that traffic does not
      arrive from the public IP.
  - Secrets never go in backups; they live in the key vaults.
- Have a written process for: takedown notices (the copyright page promises removal and notice to the user, then a
  restore window after a counter-notice), deletion and export requests (reply within 30 days; verify the
  requester controls the account, for example by having them sign in), and abuse reports.
- Google's OAuth consent screen needs the privacy policy URL (`legal/privacy.html` on your site) and the homepage.

## 4. Retention at a glance

`innkeeperd` soft-deletes: a deleted item disappears for players at once, then is erased after a grace period.

| What | Kept |
|---|---|
| deleted mail and letters | 90 days |
| deleted bulletin posts | 90 days |
| an account you are asked to delete | disabled at once, erased after 30 days (restorable in that window) |
| complaints and abuse reports | 365 days |
| sealed connection records | 90 days (`--audit-retention-days`) |
| expired sign-in sessions | 30 days |
| backups | rotate within 30 days |

Anything under a legal hold is kept until the hold ends, then the normal clock applies. Access requests include
items still inside a deletion window. Commands (names as given for the server; check `innkeeperd --help` for the
current ones): `innkeeperd account delete|restore|export`, `innkeeperd hold --account N --until DATE` and
`release`, and `--purge-now`, which is refused while a hold applies. The privacy notice states these numbers;
if you change them, change the notice and raise `termsVersion`.

## 5. Law-enforcement requests and reporting

*Not legal advice.* The server keeps a sealed record per connection (start and end time, account number, hashed
identity key, IP address), sealed with envelope encryption: AES-256-GCM per record, with the data key wrapped by an
RSA public key. The matching private key is a non-exportable key in a separate **escrow** Azure Key Vault that the
server's identity cannot access. The server cannot read the records, and every decryption is logged by Key Vault. The raw sign-in identifier is also kept sealed, so you can answer "which provider
account is this?" for one account under legal process. Records are purged after the retention period.

- **Verify the request is genuine** (a real agency, a real case, a proper legal document) and **get a lawyer**
  before disclosing anything. Be polite and slow; fake emergency and fake subpoena requests exist.
- **Preservation requests** (18 U.S.C. section 2703(f)): place a hold on the named account with the hold command
  (`innkeeperd hold --account N --until DATE`; `innkeeperd audit hold` is an alias), so its records survive the
  purge. Hold
  only what is named.
- **Decrypt only the named accounts' records**, on your own machine, not the server:
  1. Export just those records with `innkeeperd audit export --account N`.
  2. Unwrap their data keys through the escrow vault with your own Azure login (`tools/audit_unwrap.sh`, which
     uses `az keyvault key decrypt`).
  3. Open the records with `innkeeperd audit open`.

  Never decrypt casually, and never decrypt to look around. Key Vault's logs show every unwrap.
- **Keep a log of disclosures:** date, requester, the legal document, what was handed over, and who approved it.
- **Reporting apparent child sexual abuse material:** if you gain actual knowledge of apparent CSAM, US providers
  must report it to the NCMEC CyberTipline (18 U.S.C. section 2258A) and preserve the reported content. Check the
  current preservation period: it was extended to one year in 2024 (the REPORT Act). Do not view, copy or
  forward the material beyond what you are advised to do; ask a lawyer first.
- **Key handling:** two vaults.
  - The **server vault**, readable by the server's managed identity (or its service principal when
    self-hosted), holds the identity hashing key, the sign-in client secrets and a copy of the audit public key.
  - The **escrow vault** holds the audit private key and grants the server nothing. Use only your own account,
    ideally with just-in-time elevation.
  - Infrastructure as code creates the key inside the escrow vault and copies its public half to the server
    vault. `tools/audit_roundtrip.sh` then checks that a record sealed with the server's copy opens with the
    escrow key.
  - Keep soft delete and purge protection on for both vaults. A deleted key means sealed records can never be
    opened.
  - For local development there is no vault: `innkeeperd audit dev-keygen` makes a local key pair.

## 6. Beyond the site

- Personal liability: some operators use an LLC (a limited liability company) and an umbrella insurance policy.
  A lawyer can advise whether either is worth it for you.
- Keep the page's original-software wording generic (no company or product names on the legal pages), and keep
  the service from hosting the original client software.
- Revisit all of this before any launch beyond friends, and whenever you add features that collect more.
