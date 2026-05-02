Jennifer Lee(dl1317) Siddhart Sathya(ss4294)

Test Plan
----------------------
We tested the following scenarios. Each test in test_client.c corresponds
to one scenario below.

T01 – Normal login
  A client connects and sends a valid NAM. We verify the server responds
  with a MSG (welcome) rather than an ERR.

T02 – Duplicate screen name
  Two clients connect and both try to log in with the same name. We verify
  the second receives ERR code 1 (name in use).

T03 – Screen name too long
  A client sends a 33-character name (one over the 32-char limit). We
  verify the server returns ERR code 4 (too long).

T04 – Illegal character in screen name
  A client sends a name containing a space (not allowed). We verify the
  server returns ERR code 3 (illegal character).

T05 – Broadcast MSG delivery
  Two clients are logged in. One sends a MSG to #all. We verify the other
  receives it with the correct sender field and recipient #all.

T06 – Private MSG isolation
  Three clients are logged in. One sends a private MSG to a second. We
  verify the target receives it and the third client does NOT.

T07 – MSG to unknown recipient
  A logged-in client sends a MSG to a name that doesn't exist. We verify
  ERR code 2 (unknown recipient) is returned.

T08 – MSG body too long
  A client sends an 81-character message body (over the 80-char limit).
  We verify ERR code 4 (too long) is returned.

T09 – SET and WHO with status
  A client sets a status with SET. Another client issues WHO for the first
  user. We verify the response includes both the name and the status string.

T10 – WHO #all lists all users
  Two clients are logged in. One issues WHO #all. We verify both names
  appear in the response.

T11 – WHO for unknown user
  A client issues WHO for a name that doesn't exist. We verify ERR code 2
  (unknown recipient) is returned.

T12 – Clear status with empty SET
  A client sets a non-empty status then sends SET with an empty string.
  Another client then issues WHO. We verify the response reads "No status".

T13 – Disconnect removes user from room
  Two clients are logged in. One closes its connection. We wait one second
  for the server to process the hangup, then the remaining client issues
  WHO #all. We verify the disconnected user no longer appears.

Manual tests (performed interactively):
  - Verified the client's /msg, /who, /set, /quit commands work correctly.
  - Verified that private messages are displayed with "[PM from X]" prefix.
  - Verified server announcements (status changes, welcome) are prefixed
    with "[server]".
  - Verified that pressing Ctrl-D cleanly exits the client.
