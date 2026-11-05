# Manual Google-account verification client

Build `build.sh <out> --framework-res <apk> --framework-classes <archive> --core-classes <coremod>`.
The normal lifecycle wrapper emits `googleaccount.apk`.

This separate, unsigned developer test APK exercises public Android `AccountManager`
APIs against the installed authenticator. It never installs an authenticator or
substitutes credentials. Nothing initiates sign-in or token requests on launch;
the user starts each action and completes all login/consent UI manually.

Step 1 uses `addAccount` so the resulting account can be visible to this client.
This is a new account flow, not a general existing-account picker. An account
previously added from microG settings may be hidden from this client, and an
authenticator may reject adding the same account twice. A zero client-visible
count does not establish that no system account exists. Account visibility must
remain explicitly authorized; this test does not grant itself visibility.
Step 2 requests `oauth2:https://www.googleapis.com/auth/userinfo.email`, then sends
the returned access token **only** in the HTTPS Authorization header to Google's
fixed `https://www.googleapis.com/oauth2/v2/userinfo` endpoint. Standard platform
TLS/hostname validation is used, with redirects disabled. Success requires HTTP
200, a nonempty account ID and the selected account's email in the response;
neither a stored account nor a fabricated token counts as a pass. Google's
client/scope policy may still reject this
developer APK. This is not validation of Google's separate Sign-In SDK or other
applications' scopes, Play Integrity, billing or DRM.

No password, account name, token, response body or exception message is logged or
persisted by the app. Account names are shown only in the manual chooser when
multiple accounts are already visible. The underlying microG/runtime account
storage is separate. Reopen after a device restart and repeat step 2 to check
account persistence and usable authorization.
