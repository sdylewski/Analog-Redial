<?php
/*
 * add_extension.php -- creates (or reports "already exists" for) one PJSIP
 * extension in FreePBX, using the same FreePBX\modules\Core::processQuickCreate()
 * call the GraphQL "api" module and the GUI's own Quick Create form use --
 * not a hand-rolled pjsip.conf/extensions.conf edit, which FreePBX's own
 * config regeneration would overwrite on the next Apply Config anyway.
 *
 * Run inside the FreePBX container (this is where /etc/freepbx.conf and the
 * FreePBX class autoloader live): provisioner.py `docker cp`s this file in
 * and `docker exec`s php against it -- see that file for the actual HTTP
 * service this backs.
 *
 * Usage:
 *   php add_extension.php <extension> <secret> <display name>
 *   php add_extension.php <extension> --check-only
 *
 * --check-only does nothing but report whether <extension> already exists
 * (skips the secret/name validation and never calls processQuickCreate) --
 * used by provisioner.py's read-only /check endpoint.
 *
 * Always prints one line of JSON to stdout: {"ok":bool,"message":str}
 * Exit code 0 on success, 1 on any failure (bad input, already exists,
 * FreePBX rejected it, or an unexpected exception).
 *
 * Deliberately minimal: no voicemail, no user-manager account, no email --
 * this is provisioning a hardware SIP phone, not a UCP/voicemail user.
 */

function respond(bool $ok, string $message): void {
    echo json_encode(['ok' => $ok, 'message' => $message]) . "\n";
    exit($ok ? 0 : 1);
}

$extension   = $argv[1] ?? '';
$check_only  = ($argv[2] ?? '') === '--check-only';
$secret      = $check_only ? '' : ($argv[2] ?? '');
$name        = $check_only ? '' : ($argv[3] ?? '');

if (!preg_match('/^[0-9]{2,6}$/', $extension)) {
    respond(false, 'extension must be 2-6 digits');
}
if (!$check_only) {
    if (strlen($secret) < 6 || strlen($secret) > 64) {
        respond(false, 'secret must be 6-64 characters');
    }
    if ($name === '' || strlen($name) > 64) {
        respond(false, 'name must be 1-64 characters');
    }
}

$bootstrap_settings['freepbx_auth'] = false;
require_once '/etc/freepbx.conf';

try {
    $existing = \FreePBX::Core()->getDevice($extension);
    if ($check_only) {
        respond(empty($existing), empty($existing) ? "extension $extension is free" : "extension $extension already exists");
    }
    if (!empty($existing)) {
        respond(false, "extension $extension already exists");
    }

    $data = [
        'name' => $name,
        'secret' => $secret,
        'outboundcid' => '',
        'vm' => 'no', // no voicemail box for a hardware phone
    ];
    $status = \FreePBX::Core()->processQuickCreate('pjsip', $extension, $data);
    if (is_array($status) && !empty($status['status'])) {
        respond(true, "extension $extension created");
    }
    // processQuickCreate returns either an array (new-style, with a
    // 'message') or a bare bool depending on FreePBX version -- handle both.
    if ($status === true) {
        respond(true, "extension $extension created");
    }
    $msg = is_array($status) && isset($status['message']) ? $status['message'] : 'unknown error';
    respond(false, "FreePBX rejected the extension: $msg");
} catch (\Throwable $e) {
    // Best-effort cleanup so a half-created extension doesn't block a retry
    // with "already exists" -- processQuickCreate() isn't transactional.
    try {
        \FreePBX::Core()->delDevice($extension, true);
        \FreePBX::Core()->delUser($extension, true);
    } catch (\Throwable $cleanup_err) {
        // ignore -- we're already reporting the original failure below
    }
    respond(false, 'exception: ' . $e->getMessage());
}
