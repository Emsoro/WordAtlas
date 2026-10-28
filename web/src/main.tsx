import "@fontsource-variable/fraunces/opsz.css";
import "@fontsource-variable/fraunces/opsz-italic.css";
import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import App from "./App";
import "./styles/app.css";

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);

// The desktop shell (TauriCPP) loads the app straight from inside the exe, so a
// service worker there would only add a second, stale cache layer; clean up any
// registration left by a previous build instead.
const desktopShell = location.hostname === "tauricpp.app";
if (desktopShell && "serviceWorker" in navigator) {
  navigator.serviceWorker
    .getRegistrations()
    .then((list) => list.forEach((registration) => registration.unregister()))
    .catch(() => null);
}

if (import.meta.env.PROD && !desktopShell && "serviceWorker" in navigator) {
  window.addEventListener("load", () => {
    navigator.serviceWorker.register(`${import.meta.env.BASE_URL}sw.js`).catch(() => null);
  });
}
