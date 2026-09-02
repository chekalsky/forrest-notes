import SwiftUI

struct TransferProgressView: View {
    let info: TransferProgressInfo
    let deviceState: DeviceState?

    private var isActive: Bool {
        info.phase == .receiving || deviceState == .transferring
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(info.statusLine)
                    .font(.subheadline)
                Spacer()
                if isActive || info.phase == .complete {
                    Text(info.percentText)
                        .font(.subheadline.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }

            if info.phase == .failed {
                Label(info.errorMessage ?? "Transfer failed", systemImage: "exclamationmark.triangle")
                    .font(.caption)
                    .foregroundStyle(.red)
            } else if info.phase == .complete {
                ProgressView(value: 1)
                    .tint(.green)
                Text(info.bytesText)
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            } else if isActive {
                ProgressView(value: info.fraction)
                HStack {
                    Text(info.bytesText)
                    Spacer()
                    if info.chunkSeq >= 0 {
                        Text("chunk \(info.chunkSeq + 1)")
                    }
                }
                .font(.caption.monospacedDigit())
                .foregroundStyle(.secondary)
            } else if deviceState == .transferring {
                ProgressView()
                Text("Waiting for audio…")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 4)
        .animation(.easeInOut(duration: 0.2), value: info.fraction)
        .animation(.easeInOut(duration: 0.2), value: info.phase)
    }
}

#if DEBUG
#Preview("Receiving") {
    List {
        TransferProgressView(
            info: TransferProgressInfo(
                phase: .receiving,
                recordingId: 3,
                receivedBytes: 245_760,
                expectedBytes: 960_000,
                chunkSeq: 142
            ),
            deviceState: .transferring
        )
    }
}
#endif
