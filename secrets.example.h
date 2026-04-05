/**
 * @file secrets.example.h
 * @brief Template for secrets.h — fill in your own values and rename to secrets.h
 * @note This file IS committed to git as a reference. secrets.h is NOT.
 */

#ifndef SECRETS_H
#define SECRETS_H

// ==========================================================
// Firebase Configuration
// ==========================================================
#define FIREBASE_API_KEY        "YOUR_FIREBASE_API_KEY"
#define FIREBASE_DATABASE_URL   "https://YOUR_PROJECT.firebasedatabase.app"

// Firebase Auth (ESP user account)
#define ESP_USER_EMAIL          "your_email@example.com"
#define ESP_USER_PASSWORD       "your_firebase_password"

// ==========================================================
// HTTP / Web UI Credentials
// ==========================================================
#define HTTP_USERNAME_SECRET    "YourUsername"
#define HTTP_PASSWORD_SECRET    "YourPassword"
#define API_KEY_SECRET          "your_api_key"

#endif // SECRETS_H
