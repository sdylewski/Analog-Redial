# Update flow, end to end

`e2e.js` drives the web UI's "Check for updates" and "Install" in a real (headless) Chrome against a real
board, with a fake GitHub served from this computer. See the header of `e2e.js` for the command.
Covers: the update banner and "Not now", an image for another board type (refused by the board), a short
download (refused by the page), and a real install that reboots the board into the new image.
Not covered: an image that boots badly (rollback; test it by installing a build that crashes at start-up)
and the real raw.githubusercontent.com hosting (needs a public repository).
