import torch

from sumu.ai.models.basicvsrpp.basicvsrpp_gan import BasicVSRPlusPlusGan
from sumu.ai.utils import ImageTensor

class BasicvsrppMosaicRestorer:
    def __init__(self, model: BasicVSRPlusPlusGan, device: torch.device, fp16: bool, split_forward=None):
        self.model = model
        self.device: torch.device = torch.device(device)
        self.dtype = torch.float16 if fp16 else torch.float32
        # Optional TensorRT split forward (BasicVSRPlusPlusNetSplit). When set, restore()
        # runs the compiled engines instead of self.model; otherwise the PyTorch model is
        # used. self.model is always kept as the fallback (and so warmup/structure stay
        # available). See lada/restorationpipeline/basicvsrpp_sub_engines.py.
        self._split_forward = split_forward

    def warmup(self, num_frames: int = 8, size: int = 256):
        """Run one dummy forward to pay the one-time CUDA/cuDNN init cost (kernel autotune,
        lazy kernel compilation, allocator first big-block alloc) at model-load time instead
        of on the first real clip. The realtime path is very sensitive to this: the first clip
        after a (re)start must finish within the cold-start lead, or playback falls back to the
        original until the AI catches up. The spatial shape (size x size x 3) matches the
        cropped/resized clip the detector emits (MosaicDetector.clip_size, default 256). The
        frame count only needs to exercise the temporal propagation (spynet forward+backward),
        not match any real clip length, so a handful of frames is enough. Best-effort: callers
        should not let a warmup failure block model loading."""
        dummy = [torch.randint(0, 256, (size, size, 3), dtype=torch.uint8) for _ in range(num_frames)]
        self.restore(dummy)

    def restore(self, video: list[ImageTensor], max_frames=-1) -> list[ImageTensor]:
        input_frame_count = len(video)
        input_frame_shape = video[0].shape
        with torch.inference_mode():
            result = []
            inference_view = torch.stack([x.permute(2, 0, 1) for x in video], dim=0).to(device=self.device).to(dtype=self.dtype).div_(255.0).unsqueeze(0)

            if max_frames > 0:
                for i in range(0, inference_view.shape[1], max_frames):
                    output = self.model(inputs=inference_view[:, i:i + max_frames])
                    result.append(output)
                result = torch.cat(result, dim=1)
            elif self._split_forward is not None:
                # TensorRT path: the clip is already capped at the engine's max_clip_size
                # upper bound, so no batching is needed here. split_forward takes the same
                # (N,T,C,H,W) input and returns the same shape as self.model.
                result = self._split_forward(inference_view)
            else:
                result = self.model(inputs=inference_view)

            # (H, W, C[BGR]) uint8 images to (B, T, C, H, W) float in [0,1]
            result = result.squeeze(0)[:input_frame_count] # -> (T, C, H, W)
            result = result.mul_(255.0).round_().clamp_(0, 255).to(dtype=torch.uint8).permute(0, 2, 3, 1) # (T, H, W, C)
            result = list(torch.unbind(result, 0)) # (T, H, W, C) to list of (H, W, C)
            output_frame_count = len(result)
            output_frame_shape = result[0].shape
            assert input_frame_count == output_frame_count and input_frame_shape == output_frame_shape

        return result
