import sys
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.widgets import CheckButtons

CSV_PATH = "csi_logs/XXXXX.csv" # file path here
GAUSS_KERNEL_SIZE = 5
GAUSS_SIGMA = 1.0
KALMAN_Q = 0.01
KALMAN_R = 1.0
MARGIN = 3.0
SHOW_SC = [0]

HAMPEL_WINDOW = 5
HAMPEL_SIGMA = 3

def gaussian_kernel(size=5, sigma=1.0):
    assert size % 2 == 1
    ax = np.arange(size) - size // 2
    kernel = np.exp(-(ax ** 2) / (2 * sigma ** 2))
    return kernel / kernel.sum()


def apply_gaussian_smoothing(series, kernel):
    pad = len(kernel) // 2
    padded = np.pad(series, (pad, pad), mode='edge')
    return np.convolve(padded, kernel, mode='valid')


def kalman_filter_1d(series, q=0.01, r=1.0):
    n = len(series)
    x_hat = np.zeros(n)
    P = np.zeros(n)
    x_hat[0] = series[0]
    P[0] = 1.0
    for k in range(1, n):
        x_pred = x_hat[k-1]
        P_pred = P[k-1] + q

        K = P_pred / (P_pred + r)
        x_hat[k] = x_pred + K * (series[k] - x_pred)
        P[k] = (1 - K) * P_pred
    return x_hat


def hampel_filter_1d(x, window_size=5, n_sigmas=3):
    x = x.copy()
    n = len(x)
    k = 1.4826
    half = window_size // 2
    for i in range(n):
        start = max(0, i - half)
        end = min(n, i + half + 1)
        window = x[start:end]
        median = np.median(window)
        mad = k * np.median(np.abs(window - median)) + 1e-6
        if abs(x[i] - median) > n_sigmas * mad:
            x[i] = median
    return x


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else CSV_PATH
    df = pd.read_csv(path)
    ts = df["timestamp"].values
    data = df.drop(columns=["timestamp"]).astype(float).values  # (T, S)
    T, S = data.shape

    kernel = gaussian_kernel(GAUSS_KERNEL_SIZE, GAUSS_SIGMA)

    data_hampel = np.zeros_like(data)
    data_gauss = np.zeros_like(data)
    data_kalman = np.zeros_like(data)
    data_final = np.zeros_like(data)

    for sc in range(S):
        col = data[:, sc]

        col_hampel = hampel_filter_1d(col, window_size=HAMPEL_WINDOW, n_sigmas=HAMPEL_SIGMA)

        col_gauss = apply_gaussian_smoothing(col_hampel, kernel)

        col_kalman = kalman_filter_1d(col_gauss, q=KALMAN_Q, r=KALMAN_R)
        
        lower = col_kalman - MARGIN
        upper = col_kalman + MARGIN
        col_final = np.clip(col_hampel, lower, upper)

        data_hampel[:, sc] = col_hampel
        data_gauss[:, sc] = col_gauss
        data_kalman[:, sc] = col_kalman
        data_final[:, sc] = col_final

    x = np.arange(T)

    for sc in SHOW_SC:
        fig, ax = plt.subplots(figsize=(12, 5))
        plt.subplots_adjust(left=0.2)

        line_orig, = ax.plot(x, data[:, sc], label="orig", alpha=0.25)
        line_hampel, = ax.plot(x, data_hampel[:, sc], label="hampel", alpha=0.6)
        line_gauss, = ax.plot(x, data_gauss[:, sc], label="gaussian", alpha=0.8)
        line_kalman, = ax.plot(x, data_kalman[:, sc], label="kalman", linewidth=2)
        line_final, = ax.plot(x, data_final[:, sc], label="final", linewidth=1.5)

        ax.set_title(f"CSI: Hampel → Gaussian → Kalman → Clamp (sc_{sc})")
        ax.set_xlabel("packet index")
        ax.set_ylabel("amplitude")

        rax = plt.axes([0.02, 0.2, 0.12, 0.3])  # [left, bottom, width, height]
        labels = ["orig", "hampel", "gaussian", "kalman", "final"]
        visibility = [True, True, True, True, True]
        check = CheckButtons(rax, labels, visibility)

        line_dict = {
            "orig": line_orig,
            "hampel": line_hampel,
            "gaussian": line_gauss,
            "kalman": line_kalman,
            "final": line_final,
        }

        def func(label):
            line = line_dict[label]
            vis = not line.get_visible()
            line.set_visible(vis)
            plt.draw()

        check.on_clicked(func)

        plt.tight_layout()
        plt.show()

    out_df = pd.DataFrame(data_final, columns=df.columns[1:])
    out_df.insert(0, "timestamp", ts)
    out_df.to_csv("preprocessing_XXX.csv", index=False)

if __name__ == "__main__":
    main()