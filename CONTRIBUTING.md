# Contributing to Smart Weather Station IoT

Thank you for your interest in contributing to the **Smart Weather Station IoT** project! We welcome contributions, bug fixes, feature enhancements, and documentation improvements.

---

## 🛠️ Code of Conduct

Please be respectful, collaborative, and considerate of others when submitting issues or pull requests.

---

## 🚀 How to Contribute

### 1. Reporting Issues & Bugs
- Check the [Issues tab](https://github.com/dome-94/smart-weather-station-iot/issues) to verify if the issue has already been reported.
- If not, create a new issue detailing:
  - Clear title and detailed description of the behavior.
  - Hardware specifications (ESP8266 board model, LoRa frequency, sensor wiring).
  - Software environment (Arduino IDE version, Python version, OS).
  - Steps to reproduce and relevant error logs/serial monitor output.

### 2. Suggesting Enhancements
- Open a feature request issue with a detailed explanation of the proposed feature, user benefits, and any implementation considerations.

### 3. Submitting Pull Requests (PR)
1. **Fork the Repository**: Create your personal fork on GitHub.
2. **Create a Feature Branch**:
   ```bash
   git checkout -b feature/your-feature-name
   # or for bug fixes:
   git checkout -b fix/issue-description
   ```
3. **Implement Changes**:
   - Follow clean code practices.
   - For C++/Arduino code: keep variables descriptive and comment hardware pin assignments.
   - For Python ML code: format with PEP 8 standards and test inference runs cleanly.
4. **Compile UI Changes** (if applicable):
   If you modified `data/viewWeatherStation.html`, run the compiler to update `index.html` and `firmware/gateway_node/weather_html.h`:
   ```bash
   python compile_html.py
   ```
5. **Commit Your Work**:
   Write conventional, descriptive commit messages:
   ```bash
   git commit -m "feat(ml): add humidity moving average lag feature"
   ```
6. **Push and Open PR**:
   ```bash
   git push origin feature/your-feature-name
   ```
   Submit the pull request against the `main` branch with a concise summary of changes and reference any related issues.

---

## 📜 Development Guidelines

- **Zero CDN Dependencies**: The web dashboard must remain self-contained with no external CSS/JS CDN dependencies to maintain 100% offline functionality in Gateway SoftAP mode.
- **Payload Constraints**: When modifying LoRa packet schemas, ensure payload stays under the 15-byte compact binary limit to maximize battery longevity and minimize RF airtime.
- **Python Dependencies**: Keep package dependencies in `ml_engine/requirements.txt` minimal and pinned to stable releases.

---

Thank you for contributing to open-source IoT and climate intelligence!
