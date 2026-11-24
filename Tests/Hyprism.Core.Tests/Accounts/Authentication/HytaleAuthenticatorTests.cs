// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

using Hyprism.Core.Models;
using Hyprism.Core.Infrastructure;
using Hyprism.Core.Accounts;
using System.Net;
using System.Text.Json;

namespace Hyprism.Core.Tests.Accounts.Authentication;

public sealed class HytaleAuthenticatorTests
{
    [Fact]
    public void SessionJson_WritesPascalCaseAndReadsLegacySnakeCase()
    {
        var session = new HytaleAuthSession
        {
            AccessToken = "access",
            RefreshToken = "refresh",
            ExpiresAt = new DateTime(2026, 8, 24, 12, 0, 0, DateTimeKind.Utc),
            SessionToken = "session",
            IdentityToken = "identity",
            Username = "Player",
            UUID = "550e8400-e29b-41d4-a716-446655440000",
            AccountOwnerId = "owner"
        };

        var json = JsonSerializer.Serialize(session);
        using var document = JsonDocument.Parse(json);
        Assert.All(
            document.RootElement.EnumerateObject(),
            property => Assert.True(char.IsUpper(property.Name[0]), property.Name));

        var restored = JsonSerializer.Deserialize<HytaleAuthSession>("""
            {
              "access_token": "legacy-access",
              "refresh_token": "legacy-refresh",
              "expires_at": "2026-08-24T12:00:00Z",
              "session_token": "legacy-session",
              "identity_token": "legacy-identity",
              "username": "LegacyPlayer",
              "uuid": "660e8400-e29b-41d4-a716-446655440000",
              "account_owner_id": "legacy-owner"
            }
            """);

        Assert.NotNull(restored);
        Assert.Equal("legacy-access", restored.AccessToken);
        Assert.Equal("legacy-refresh", restored.RefreshToken);
        Assert.Equal("legacy-session", restored.SessionToken);
        Assert.Equal("legacy-identity", restored.IdentityToken);
        Assert.Equal("LegacyPlayer", restored.Username);
        Assert.Equal("660e8400-e29b-41d4-a716-446655440000", restored.UUID);
        Assert.Equal("legacy-owner", restored.AccountOwnerId);
    }

    [Fact]
    public async Task LoginAsync_PresentsGeneratedAuthorizationUriThroughHostCallback()
    {
        var appDir = Path.Combine(Path.GetTempPath(), $"hyprism-auth-{Guid.NewGuid():N}");
        Directory.CreateDirectory(appDir);

        try
        {
            var config = new Mock<IConfigStore>();
            config.SetupGet(service => service.Configuration).Returns(new Config());
            using var httpClient = new HttpClient();
            var auth = new HytaleAuthenticator(httpClient, appDir, config.Object);
            Uri? presentedUri = null;

            var session = await auth.LoginAsync((uri, _) =>
            {
                presentedUri = uri;
                return Task.FromResult(false);
            });

            Assert.Null(session);
            Assert.NotNull(presentedUri);
            Assert.Equal(Uri.UriSchemeHttps, presentedUri.Scheme);
            Assert.Equal("oauth.accounts.hytale.com", presentedUri.Host);
            Assert.Contains("code_challenge=", presentedUri.Query, StringComparison.Ordinal);
            Assert.Contains("state=", presentedUri.Query, StringComparison.Ordinal);
            Assert.Contains(
                "&scope=openid%20offline%20auth%3Alauncher&",
                presentedUri.AbsoluteUri,
                StringComparison.Ordinal);
        }
        finally
        {
            Directory.Delete(appDir, recursive: true);
        }
    }

    [Fact]
    public async Task LoginAsync_ServesOfficialCallbackBeforePresentingAuthorizationUri()
    {
        var appDir = Path.Combine(Path.GetTempPath(), $"hyprism-auth-{Guid.NewGuid():N}");
        Directory.CreateDirectory(appDir);

        try
        {
            var config = new Mock<IConfigStore>();
            config.SetupGet(service => service.Configuration).Returns(new Config());
            using var httpClient = new HttpClient();
            var auth = new HytaleAuthenticator(httpClient, appDir, config.Object);

            var session = await auth.LoginAsync(async (uri, cancellationToken) =>
            {
                var state = GetQueryValue(uri, "state");
                using var stateDocument = JsonDocument.Parse(Convert.FromBase64String(state));
                var port = stateDocument.RootElement.GetProperty("port").GetString();
                var callbackState = stateDocument.RootElement.GetProperty("state").GetString();
                Assert.False(string.IsNullOrWhiteSpace(port));
                Assert.False(string.IsNullOrWhiteSpace(callbackState));

                using var callbackClient = new HttpClient();
                using var response = await callbackClient.GetAsync(
                    $"http://127.0.0.1:{port}/authorization-callback" +
                    $"?error=access_denied&state={Uri.EscapeDataString(callbackState)}",
                    cancellationToken);

                Assert.Equal(System.Net.HttpStatusCode.InternalServerError, response.StatusCode);
                return true;
            });

            Assert.Null(session);
        }
        finally
        {
            Directory.Delete(appDir, recursive: true);
        }
    }

    [Fact]
    public async Task LoginAsync_UsesHostRendererForSuccessfulCallbackPage()
    {
        var appDir = Path.Combine(Path.GetTempPath(), $"hyprism-auth-{Guid.NewGuid():N}");
        Directory.CreateDirectory(appDir);

        try
        {
            var config = new Mock<IConfigStore>();
            config.SetupGet(service => service.Configuration).Returns(new Config());
            var renderer = new Mock<IOAuthCallbackPageRenderer>();
            renderer
                .Setup(value => value.Render(true, It.IsAny<string>()))
                .Returns("<!doctype html><html><body>branded-success-page</body></html>");
            using var httpClient = new HttpClient();
            var auth = new HytaleAuthenticator(httpClient, appDir, config.Object, renderer.Object);

            var session = await auth.LoginAsync(async (uri, cancellationToken) =>
            {
                var state = GetQueryValue(uri, "state");
                using var stateDocument = JsonDocument.Parse(Convert.FromBase64String(state));
                var port = stateDocument.RootElement.GetProperty("port").GetString();
                var callbackState = stateDocument.RootElement.GetProperty("state").GetString();
                Assert.False(string.IsNullOrWhiteSpace(port));
                Assert.False(string.IsNullOrWhiteSpace(callbackState));

                using var callbackClient = new HttpClient();
                using var response = await callbackClient.GetAsync(
                    $"http://127.0.0.1:{port}/authorization-callback" +
                    $"?code=test-code&state={Uri.EscapeDataString(callbackState!)}",
                    cancellationToken);
                var responseHtml = await response.Content.ReadAsStringAsync(cancellationToken);

                Assert.Equal(System.Net.HttpStatusCode.OK, response.StatusCode);
                Assert.Contains("branded-success-page", responseHtml, StringComparison.Ordinal);
                return false;
            });

            Assert.Null(session);
            renderer.Verify(
                value => value.Render(
                    true,
                    "Authorization completed successfully"),
                Times.Once);
        }
        finally
        {
            Directory.Delete(appDir, recursive: true);
        }
    }

    [Fact]
    public async Task LoginAsync_DeliversCallbackPageBeforeCompletingAuthentication()
    {
        var appDir = Path.Combine(Path.GetTempPath(), $"hyprism-auth-{Guid.NewGuid():N}");
        Directory.CreateDirectory(appDir);

        try
        {
            var config = new Mock<IConfigStore>();
            config.SetupGet(service => service.Configuration).Returns(new Config());
            var renderer = new Mock<IOAuthCallbackPageRenderer>();
            renderer
                .Setup(value => value.Render(true, It.IsAny<string>()))
                .Returns("<!doctype html><html><body>branded-success-page</body></html>");
            using var httpClient = new HttpClient(new SuccessfulOAuthHandler());
            var auth = new HytaleAuthenticator(httpClient, appDir, config.Object, renderer.Object);
            using var callbackClient = new HttpClient();
            Task<string>? callbackResponseTask = null;

            var session = await auth.LoginAsync((uri, _) =>
            {
                var state = GetQueryValue(uri, "state");
                using var stateDocument = JsonDocument.Parse(Convert.FromBase64String(state));
                var port = stateDocument.RootElement.GetProperty("port").GetString();
                var callbackState = stateDocument.RootElement.GetProperty("state").GetString();
                Assert.False(string.IsNullOrWhiteSpace(port));
                Assert.False(string.IsNullOrWhiteSpace(callbackState));

                callbackResponseTask = callbackClient.GetStringAsync(
                    $"http://127.0.0.1:{port}/authorization-callback" +
                    $"?code=test-code&state={Uri.EscapeDataString(callbackState!)}");
                return Task.FromResult(true);
            });

            Assert.NotNull(session);
            Assert.Equal("TestPlayer", session.Username);
            Assert.NotNull(callbackResponseTask);
            var responseHtml = await callbackResponseTask;

            Assert.Contains("branded-success-page", responseHtml, StringComparison.Ordinal);
            renderer.Verify(
                value => value.Render(
                    true,
                    "Authorization completed successfully"),
                Times.Once);
        }
        finally
        {
            Directory.Delete(appDir, recursive: true);
        }
    }

    [Fact]
    public async Task LoginAsync_RejectsMissingHostCallback()
    {
        var appDir = Path.Combine(Path.GetTempPath(), $"hyprism-auth-{Guid.NewGuid():N}");
        Directory.CreateDirectory(appDir);

        try
        {
            var config = new Mock<IConfigStore>();
            config.SetupGet(service => service.Configuration).Returns(new Config());
            using var httpClient = new HttpClient();
            var auth = new HytaleAuthenticator(httpClient, appDir, config.Object);

            await Assert.ThrowsAsync<ArgumentNullException>(
                () => auth.LoginAsync(null!));
        }
        finally
        {
            Directory.Delete(appDir, recursive: true);
        }
    }

    private static string GetQueryValue(Uri uri, string name)
    {
        foreach (var pair in uri.Query.TrimStart('?').Split('&', StringSplitOptions.RemoveEmptyEntries))
        {
            var separator = pair.IndexOf('=');
            var encodedName = separator >= 0 ? pair[..separator] : pair;
            if (!string.Equals(Uri.UnescapeDataString(encodedName), name, StringComparison.Ordinal))
                continue;

            var encodedValue = separator >= 0 ? pair[(separator + 1)..] : string.Empty;
            return Uri.UnescapeDataString(encodedValue.Replace("+", " ", StringComparison.Ordinal));
        }

        throw new InvalidOperationException($"Query parameter '{name}' was not found");
    }

    [Fact]
    public async Task GetValidSessionAsync_KeepsTheSavedSessionWhenTheRefreshCannotReachTheServer()
    {
        await WithExpiredSessionAsync(
            new TokenEndpointHandler(_ => throw new HttpRequestException("network is unreachable")),
            async (auth, sessionPath, _) =>
            {
                Assert.Null(await auth.GetValidSessionAsync());

                Assert.True(File.Exists(sessionPath));
                Assert.NotNull(auth.CurrentSession);
                Assert.Equal("old-refresh", auth.CurrentSession.RefreshToken);
            });
    }

    [Fact]
    public async Task GetValidSessionAsync_KeepsTheSavedSessionOnAServerError()
    {
        await WithExpiredSessionAsync(
            new TokenEndpointHandler(_ => new HttpResponseMessage(HttpStatusCode.InternalServerError)),
            async (auth, sessionPath, _) =>
            {
                Assert.Null(await auth.GetValidSessionAsync());
                Assert.True(File.Exists(sessionPath));
            });
    }

    [Fact]
    public async Task GetValidSessionAsync_LogsOutWhenTheServerRejectsTheRefreshToken()
    {
        await WithExpiredSessionAsync(
            new TokenEndpointHandler(_ => new HttpResponseMessage(HttpStatusCode.BadRequest)
            {
                Content = new StringContent("""{"error":"invalid_grant"}""")
            }),
            async (auth, sessionPath, _) =>
            {
                Assert.Null(await auth.GetValidSessionAsync());
                Assert.False(File.Exists(sessionPath));
                Assert.Null(auth.CurrentSession);
            });
    }

    [Fact]
    public async Task GetValidSessionAsync_RefreshesOnceForConcurrentCallers()
    {
        var handler = new TokenEndpointHandler(_ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent("""{"access_token":"new-access","refresh_token":"new-refresh","expires_in":3600}""")
        });
        await WithExpiredSessionAsync(handler, async (auth, sessionPath, _) =>
        {
            var sessions = await Task.WhenAll(Enumerable.Range(0, 5).Select(_ => auth.GetValidSessionAsync()));

            Assert.Equal(1, handler.Calls);
            Assert.All(sessions, session => Assert.Equal("new-access", session?.AccessToken));
            Assert.Contains("new-refresh", await File.ReadAllTextAsync(sessionPath), StringComparison.Ordinal);
            Assert.False(File.Exists(sessionPath + ".tmp"));
        });
    }

    private static async Task WithExpiredSessionAsync(
        TokenEndpointHandler handler,
        Func<HytaleAuthenticator, string, string, Task> test)
    {
        var appDir = Path.Combine(Path.GetTempPath(), $"hyprism-auth-{Guid.NewGuid():N}");
        Directory.CreateDirectory(appDir);
        try
        {
            var sessionPath = Path.Combine(appDir, "HytaleSession.json");
            await File.WriteAllTextAsync(sessionPath, JsonSerializer.Serialize(new HytaleAuthSession
            {
                AccessToken = "old-access",
                RefreshToken = "old-refresh",
                ExpiresAt = DateTime.UtcNow.AddHours(-1),
                Username = "Player",
                UUID = "550e8400-e29b-41d4-a716-446655440000",
            }));
            var config = new Mock<IConfigStore>();
            config.SetupGet(service => service.Configuration).Returns(new Config());
            using var httpClient = new HttpClient(handler);
            var auth = new HytaleAuthenticator(httpClient, appDir, config.Object);
            Assert.NotNull(auth.CurrentSession);

            await test(auth, sessionPath, appDir);
        }
        finally
        {
            Directory.Delete(appDir, recursive: true);
        }
    }

    /// <summary>Answers the OAuth token endpoint, slowly enough for concurrent callers to overlap.</summary>
    private sealed class TokenEndpointHandler(Func<HttpRequestMessage, HttpResponseMessage> respond) : HttpMessageHandler
    {
        private int _calls;

        public int Calls => _calls;

        protected override async Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request,
            CancellationToken cancellationToken)
        {
            Assert.Equal("oauth.accounts.hytale.com", request.RequestUri?.Host);
            Interlocked.Increment(ref _calls);
            await Task.Delay(50, cancellationToken);
            return respond(request);
        }
    }

    private sealed class SuccessfulOAuthHandler : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request,
            CancellationToken cancellationToken)
        {
            var responseBody = request.RequestUri?.Host switch
            {
                "oauth.accounts.hytale.com" => """
                    {"access_token":"access-token","refresh_token":"refresh-token","expires_in":3600}
                    """,
                "account-data.hytale.com" => """
                    {"owner":"owner","profiles":[{"username":"TestPlayer","uuid":"550e8400-e29b-41d4-a716-446655440000"}]}
                    """,
                "sessions.hytale.com" => """
                    {"sessionToken":"session-token","identityToken":"identity-token"}
                    """,
                "launcher.hytale.com" => """
                    {"version":"test"}
                    """,
                _ => throw new InvalidOperationException($"Unexpected HTTP request: {request.RequestUri}")
            };

            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent(responseBody)
            });
        }
    }
}
