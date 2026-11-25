// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Credential redaction vectors shared by the native sink (tests/logging/contracts.cc)
// and the Java one (tests/logging/LogRedactionTest.java, which parses this file).
// One REDACTION_VECTOR(input, expected) per line; escapes: \n \t \" \\ only.
// The secrets are synthetic.
REDACTION_VECTOR("GmsAuthenticator getAuthToken: ya29.a0Aehf-SYNTHETIC_secret.123", "GmsAuthenticator getAuthToken: <redacted>")
REDACTION_VECTOR("access granted ya29.a0SyntheticAccess-Token_value end", "access granted ya29.<redacted> end")
REDACTION_VECTOR("Outgoing message: LoginRequest{id=chrome-1, domain=mcs.android.com, user=4242, resource=99, auth_token=8754321098765, device_id=android-77}", "Outgoing message: LoginRequest{id=chrome-1, domain=mcs.android.com, user=4242, resource=99, auth_token=<redacted>, device_id=android-77}")
REDACTION_VECTOR("refresh 1//0gSyntheticRefreshTokenValue-_abc done", "refresh 1//<redacted> done")
REDACTION_VECTOR("ratio 1//2 and x11//0gSyntheticRefreshTokenValue stay", "ratio 1//2 and x11//0gSyntheticRefreshTokenValue stay")
REDACTION_VECTOR("Token=aas_et/AKppINSyntheticMaster== Expiry=0", "Token=<redacted> Expiry=0")
REDACTION_VECTOR("master aas_et/AKppINSyntheticMaster== kept", "master aas_et/<redacted> kept")
REDACTION_VECTOR("exchange oauth2_4/0AeaSyntheticCode next", "exchange oauth2_4/<redacted> next")
REDACTION_VECTOR("Authorization: Bearer abc.SYNTHETIC-def\nnext line stays", "Authorization: <redacted>\nnext line stays")
REDACTION_VECTOR("header bearer xyzSYNTHETIC123 sent", "header bearer <redacted> sent")
REDACTION_VECTOR("{\"access_token\": \"ya29.SYNTHETIC\", \"expires_in\": 3599, \"refresh_token\":\"1//SYNTHETIC\", \"id_token\" : \"eyJhbGciOi.eyJzdWIi.sig\"}", "{\"access_token\": \"<redacted>\", \"expires_in\": 3599, \"refresh_token\":\"<redacted>\", \"id_token\" : \"<redacted>\"}")
REDACTION_VECTOR("POST /auth?Email=a%40b.com&Passwd=hunter2SYNTHETIC&client_secret=s3cr3tSYNTHETIC&service=ac2dm", "POST /auth?Email=a%40b.com&Passwd=<redacted>&client_secret=<redacted>&service=ac2dm")
REDACTION_VECTOR("Cookie: SID=abcSYNTHETIC; HSID=def\nSet-Cookie: x=ySYNTHETIC; Path=/", "Cookie: <redacted>\nSet-Cookie: <redacted>")
REDACTION_VECTOR("שלום password=סוד123SYNTHETIC עולם ya29.אבג", "שלום password=<redacted> עולם ya29.אבג")
REDACTION_VECTOR("securityToken: 1234567890123 androidId=3", "securityToken: <redacted> androidId=3")
REDACTION_VECTOR("authToken='abc SYNTHETIC def' ok", "authToken='<redacted>' ok")
REDACTION_VECTOR("line one\n  password = SYNTHETIC\tline two\napiKey=SYNTHETIC", "line one\n  password = <redacted>\tline two\napiKey=<redacted>")
REDACTION_VECTOR("auth_token=<redacted> ya29.<redacted> Bearer <redacted> Authorization: <redacted>", "auth_token=<redacted> ya29.<redacted> Bearer <redacted> Authorization: <redacted>")
REDACTION_VECTOR("tokens=5 tokenizer: ok, monkey=banana, std::string token_count", "tokens=5 tokenizer: ok, monkey=banana, std::string token_count")
REDACTION_VECTOR("", "")
