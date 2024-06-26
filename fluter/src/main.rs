use std::{slice, time::Duration};

use anyhow::{bail, Context, Result};
use clap::Parser;
// use nix::{
//     fcntl::OFlag,
//     libc::O_CREAT,
//     sys::{
//         mman::{mmap, munmap, shm_open, shm_unlink, MapFlags, ProtFlags},
//         stat::Mode,
//     },
// };
use shared_memory::ShmemConf;
use tokio::{io::AsyncWriteExt, net::TcpStream, signal, time};

use args::Args;
use tracing::{debug, info, instrument, trace};

mod args;

// Width and height
const HEADER_SIZE: usize = 2 * std::mem::size_of::<u16>();

#[tokio::main]
async fn main() -> Result<(), anyhow::Error> {
    let args = Args::parse();

    tracing_subscriber::fmt().init();

    // let shm = shm_open(
    //     args.shared_memory_name.into(),
    //     OFlag::O_RDWR, //create exclusively (error if collision) and read/write to allow resize
    //     Mode::S_IRUSR | Mode::S_IWUSR, //Permission allow user+rw
    // );

    let shared_memory = ShmemConf::new()
        // .size(HEADER_SIZE)
        .os_id(&args.shared_memory_name)
        .open()
        .with_context(|| {
            format!(
                "Failed to open shared memory with the OS id \"{}\" (propably at at location /dev/shm/{}). Is the backend running?",
                args.shared_memory_name, args.shared_memory_name
            )
        })?;

    debug!(size = shared_memory.len(), "Loaded shared memory");
    if shared_memory.len() < HEADER_SIZE {
        bail!(
            "Invalid shared memory length. It needs to have at least a length of {HEADER_SIZE} bytes for the header,
                but it only has {} bytes.",
                shared_memory.len());
    }
    let size_ptr = shared_memory.as_ptr() as *const u16;
    let width = unsafe { *size_ptr };
    let height = unsafe { *size_ptr.add(1) };

    if width == 0 || height == 0 {
        bail!("The size of the framebuffer was ({width}, {height}). The axis need to be non-null");
    }
    info!(width, height, "Found existing framebuffer");

    let fb: &mut [u32] = unsafe {
        slice::from_raw_parts_mut(
            shared_memory.as_ptr().add(4) as _,
            width as usize * height as usize,
        )
    };

    info!(
        drawing_threads = args.drawing_threads,
        "Starting drawing threads"
    );
    let thread_chunk_size = (fb.len() / args.drawing_threads as usize) + 1;
    let mut index = 0;
    for fb_slice in fb.chunks_mut(thread_chunk_size) {
        let start_x = (index % width as usize) as u16;
        let start_y = (index / height as usize) as u16;
        index += fb_slice.len();

        let sink = TcpStream::connect(&args.pixelflut_sink)
            .await
            .with_context(|| {
                format!(
                    "Failed to connect to Pixelflut sink at {}",
                    &args.pixelflut_sink
                )
            })?;

        tokio::spawn(drawing_thread(
            fb_slice, sink, args.fps, start_x, start_y, width, height,
        ));
    }

    info!("Waiting for Ctrl-C...");
    signal::ctrl_c().await?;
    info!("Exiting...");

    Ok(())
}

#[instrument(skip_all, fields(start_x = start_x, start_y = start_y))]
async fn drawing_thread(
    fb_slice: &mut [u32],
    mut sink: TcpStream,
    fps: u32,
    start_x: u16,
    start_y: u16,
    width: u16,
    height: u16,
) -> Result<()> {
    debug!(pixels = fb_slice.len(), "Starting fluting pixels");
    let mut interval = time::interval(Duration::from_micros(1_000_000 / fps as u64));

    loop {
        let start = std::time::Instant::now();
        let mut x = start_x;
        let mut y = start_y;

        for rgba in fb_slice.iter_mut() {
            // Ignore alpha channel
            let rgb = *rgba >> 8;

            // Only send pixels that
            // 1.) The server is responsible for
            // 2.) Have changed since the last flush
            if rgb != 0 {
                sink.write_all(format!("PX {x} {y} {rgb:06x}\n").as_bytes())
                    .await
                    .context("Failed to write to Pixelflut sink")?;

                // Reset color back, so that we don't send the same color twice
                *rgba = 0;
            }

            x += 1;
            if x >= width {
                x = 0;
                y += 1;
                if y >= height {
                    // warn!(x, y, width, height, "x and y run over the fb bounds. This should not happen, as no thread should get work to do that");
                    // break;
                }
            }
        }

        let elapsed = start.elapsed();
        trace!(
            ?elapsed,
            duty_cycle =
                (elapsed.as_micros() as f32 / interval.period().as_micros() as f32 * 100.0).ceil(),
            "Loop completed",
        );

        sink.flush()
            .await
            .context("Failed to flush to Pixelflut sink")?;

        interval.tick().await;
    }
}
