# web_auth: signed admin requests for the C3/C6 web pages. Pure-C core
# (web_auth.c, host-tested in test/test_web_auth.c) + ESP glue (web_auth_esp.c:
# PSA HMAC-SHA256, POST /api/session, GET /auth.js) + the page side (www/auth.js,
# test/test_web_auth.mjs) + the build-time key (project_include.cmake:
# web_auth_admin_key(), admin_key.py). esp_hw_support provides the hardware RNG.
idf_component_register(SRCS "web_auth.c" "web_auth_esp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server mbedtls esp_hw_support
                       EMBED_FILES "www/auth.js")
