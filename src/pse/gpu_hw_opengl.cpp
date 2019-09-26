#include "gpu_hw_opengl.h"
#include "YBaseLib/Assert.h"
#include "YBaseLib/Log.h"
#include "host_interface.h"
#include "imgui.h"
#include "system.h"
Log_SetChannel(GPU_HW_OpenGL);

GPU_HW_OpenGL::GPU_HW_OpenGL() : GPU_HW() {}

GPU_HW_OpenGL::~GPU_HW_OpenGL()
{
  DestroyFramebuffer();
}

bool GPU_HW_OpenGL::Initialize(System* system, DMA* dma, InterruptController* interrupt_controller, Timers* timers)
{
  if (!GPU_HW::Initialize(system, dma, interrupt_controller, timers))
    return false;

  CreateFramebuffer();
  CreateVertexBuffer();
  if (!CompilePrograms())
    return false;

  return true;
}

void GPU_HW_OpenGL::Reset()
{
  GPU_HW::Reset();

  ClearFramebuffer();
}

void GPU_HW_OpenGL::RenderUI()
{
  GPU_HW::RenderUI();

  if (ImGui::Begin("GL Render Statistics"))
  {
    ImGui::Columns(2);

    ImGui::TextUnformatted("Texture Page Updates:");
    ImGui::NextColumn();
    ImGui::Text("%u", m_stats.num_vram_read_texture_updates);
    ImGui::NextColumn();

    ImGui::TextUnformatted("Batches Drawn:");
    ImGui::NextColumn();
    ImGui::Text("%u", m_stats.num_batches);
    ImGui::NextColumn();

    ImGui::TextUnformatted("Vertices Drawn: ");
    ImGui::NextColumn();
    ImGui::Text("%u", m_stats.num_vertices);
    ImGui::NextColumn();

    ImGui::Columns(1);
  }

  ImGui::End();

  m_stats = {};
}

void GPU_HW_OpenGL::InvalidateVRAMReadCache()
{
  m_vram_read_texture_dirty = true;
}

std::tuple<s32, s32> GPU_HW_OpenGL::ConvertToFramebufferCoordinates(s32 x, s32 y)
{
  return std::make_tuple(x, static_cast<s32>(static_cast<s32>(VRAM_HEIGHT) - y));
}

void GPU_HW_OpenGL::CreateFramebuffer()
{
  m_framebuffer_texture =
    std::make_unique<GL::Texture>(VRAM_WIDTH, VRAM_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false);

  glGenFramebuffers(1, &m_framebuffer_fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_framebuffer_texture->GetGLId(), 0);
  Assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

  m_vram_read_texture =
    std::make_unique<GL::Texture>(VRAM_WIDTH, VRAM_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false);
  glGenFramebuffers(1, &m_vram_read_fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, m_vram_read_fbo_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_vram_read_texture->GetGLId(), 0);
  Assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
}

void GPU_HW_OpenGL::ClearFramebuffer()
{
  // TODO: get rid of the FBO switches
  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  m_system->GetHostInterface()->SetDisplayTexture(m_framebuffer_texture.get(), 0, 0, VRAM_WIDTH, VRAM_HEIGHT);
}

void GPU_HW_OpenGL::DestroyFramebuffer()
{
  glDeleteFramebuffers(1, &m_vram_read_fbo_id);
  m_vram_read_fbo_id = 0;
  m_vram_read_texture.reset();

  glDeleteFramebuffers(1, &m_framebuffer_fbo_id);
  m_framebuffer_fbo_id = 0;
  m_framebuffer_texture.reset();
}

void GPU_HW_OpenGL::CreateVertexBuffer()
{
  glGenBuffers(1, &m_vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, VERTEX_BUFFER_SIZE, nullptr, GL_STREAM_DRAW);

  glGenVertexArrays(1, &m_vao_id);
  glBindVertexArray(m_vao_id);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glVertexAttribIPointer(0, 2, GL_INT, sizeof(HWVertex), reinterpret_cast<void*>(offsetof(HWVertex, x)));
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, true, sizeof(HWVertex),
                        reinterpret_cast<void*>(offsetof(HWVertex, color)));
  glVertexAttribPointer(2, 2, GL_UNSIGNED_BYTE, true, sizeof(HWVertex),
                        reinterpret_cast<void*>(offsetof(HWVertex, texcoord)));
  glBindVertexArray(0);

  glGenVertexArrays(1, &m_attributeless_vao_id);
}

bool GPU_HW_OpenGL::CompilePrograms()
{
  for (u32 textured = 0; textured < 2; textured++)
  {
    for (u32 blending = 0; blending < 2; blending++)
    {
      for (u32 format = 0; format < 3; format++)
      {
        // TODO: eliminate duplicate shaders here
        if (!CompileProgram(m_render_programs[textured][blending][format], ConvertToBoolUnchecked(textured),
                            ConvertToBoolUnchecked(blending), static_cast<TextureColorMode>(format)))
        {
          return false;
        }
      }
    }
  }

  return true;
}

bool GPU_HW_OpenGL::CompileProgram(GL::Program& prog, bool textured, bool blending, TextureColorMode texture_color_mode)
{
  const std::string vs = GenerateVertexShader(textured);
  const std::string fs = GenerateFragmentShader(textured, blending, texture_color_mode);
  if (!prog.Compile(vs.c_str(), fs.c_str()))
    return false;

  prog.BindAttribute(0, "a_pos");
  prog.BindAttribute(1, "a_col0");
  if (textured)
    prog.BindAttribute(2, "a_tex0");

  prog.BindFragData(0, "o_col0");

  if (!prog.Link())
    return false;

  prog.Bind();
  prog.RegisterUniform("u_pos_offset");
  prog.Uniform2i(0, 0, 0);

  if (textured)
  {
    prog.RegisterUniform("samp0");
    prog.RegisterUniform("u_texture_page_base");
    prog.RegisterUniform("u_texture_palette_base");
    prog.Uniform1i(1, 0);
  }

  return true;
}

void GPU_HW_OpenGL::SetProgram()
{
  const GL::Program& prog =
    m_render_programs[BoolToUInt32(m_batch.texture_enable)][BoolToUInt32(m_batch.texture_blending_enable)]
                     [static_cast<u32>(m_batch.texture_color_mode)];
  prog.Bind();

  prog.Uniform2i(0, m_drawing_offset.x, m_drawing_offset.y);

  if (m_batch.texture_enable)
  {
    m_vram_read_texture->Bind();
    prog.Uniform2i(2, m_batch.texture_page_x, m_batch.texture_page_y);
    prog.Uniform2i(3, m_batch.texture_palette_x, m_batch.texture_palette_y);
  }
}

void GPU_HW_OpenGL::SetViewport()
{
  glViewport(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
}

void GPU_HW_OpenGL::SetScissor()
{
  int left, top, right, bottom;
  CalcScissorRect(&left, &top, &right, &bottom);

  const int width = right - left;
  const int height = bottom - top;
  const int x = left;
  const int y = VRAM_HEIGHT - bottom;

  Log_DebugPrintf("SetScissor: (%d-%d, %d-%d)", x, x + width, y, y + height);
  glScissor(x, y, width, height);
}

void GPU_HW_OpenGL::SetBlendState()
{
  struct BlendVars
  {
    GLenum src_factor;
    GLenum func;
    GLenum dst_factor;
    float color;
  };
  static const std::array<BlendVars, 4> blend_vars = {{
    {GL_CONSTANT_COLOR, GL_FUNC_ADD, GL_CONSTANT_COLOR, 0.5f}, // B/2 + F/2
    {GL_ONE, GL_FUNC_ADD, GL_ONE, -1.0f},                      // B + F
    {GL_ONE, GL_FUNC_REVERSE_SUBTRACT, GL_ONE, -1.0f},         // B - F
    {GL_CONSTANT_COLOR, GL_FUNC_ADD, GL_ONE, 0.25f}            // B + F/4
  }};

  if (!m_batch.transparency_enable)
  {
    glDisable(GL_BLEND);
    return;
  }

  const BlendVars& vars = blend_vars[static_cast<u8>(m_batch.transparency_mode)];
  glEnable(GL_BLEND);
  glBlendFuncSeparate(vars.src_factor, vars.dst_factor, GL_ONE, GL_ZERO);
  glBlendEquationSeparate(vars.func, GL_FUNC_ADD);
  if (vars.color >= 0.0f)
    glBlendColor(vars.color, vars.color, vars.color, 1.0f);
}

void GPU_HW_OpenGL::UpdateDisplay()
{
  GPU_HW::UpdateDisplay();
  m_system->GetHostInterface()->SetDisplayTexture(m_framebuffer_texture.get(), 0, 0, VRAM_WIDTH, VRAM_HEIGHT);
}

void GPU_HW_OpenGL::ReadVRAM(u32 x, u32 y, u32 width, u32 height, void* buffer)
{
  // we need to convert RGBA8 -> RGBA5551
  std::vector<u32> temp_buffer(width * height);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_framebuffer_fbo_id);
  glReadPixels(x, VRAM_HEIGHT - y - height, width, height, GL_RGBA, GL_UNSIGNED_BYTE, temp_buffer.data());

  // reverse copy because of lower-left origin
  const u32 source_stride = width * sizeof(u32);
  const u8* source_ptr = reinterpret_cast<const u8*>(temp_buffer.data()) + (source_stride * (height - 1));
  const u32 dst_stride = width * sizeof(u16);
  u8* dst_ptr = static_cast<u8*>(buffer);
  for (u32 row = 0; row < height; row++)
  {
    const u8* source_row_ptr = source_ptr;
    u8* dst_row_ptr = dst_ptr;

    for (u32 col = 0; col < width; col++)
    {
      u32 src_col;
      std::memcpy(&src_col, source_row_ptr, sizeof(src_col));
      source_row_ptr += sizeof(src_col);

      const u16 dst_col = RGBA8888ToRGBA5551(src_col);
      std::memcpy(dst_row_ptr, &dst_col, sizeof(dst_col));
      dst_row_ptr += sizeof(dst_col);
    }

    source_ptr -= source_stride;
    dst_ptr += dst_stride;
  }
}

void GPU_HW_OpenGL::FillVRAM(u32 x, u32 y, u32 width, u32 height, u16 color)
{
  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);

  glEnable(GL_SCISSOR_TEST);
  glScissor(x, VRAM_HEIGHT - y - height, width, height);

  const auto [r, g, b, a] = RGBA8ToFloat(RGBA5551ToRGBA8888(color));
  glClearColor(r, g, b, a);
  glClear(GL_COLOR_BUFFER_BIT);

  InvalidateVRAMReadCache();
}

void GPU_HW_OpenGL::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data)
{
  std::vector<u32> rgba_data;
  rgba_data.reserve(width * height);

  // reverse copy the rows so it matches opengl's lower-left origin
  const u32 source_stride = width * sizeof(u16);
  const u8* source_ptr = static_cast<const u8*>(data) + (source_stride * (height - 1));
  for (u32 row = 0; row < height; row++)
  {
    const u8* source_row_ptr = source_ptr;

    for (u32 col = 0; col < width; col++)
    {
      u16 src_col;
      std::memcpy(&src_col, source_row_ptr, sizeof(src_col));
      source_row_ptr += sizeof(src_col);

      const u32 dst_col = RGBA5551ToRGBA8888(src_col);
      rgba_data.push_back(dst_col);
    }

    source_ptr -= source_stride;
  }

  m_framebuffer_texture->Bind();

  // lower-left origin flip happens here
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, VRAM_HEIGHT - y - height, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                  rgba_data.data());

  InvalidateVRAMReadCache();
}

void GPU_HW_OpenGL::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  glDisable(GL_SCISSOR_TEST);

  // lower-left origin flip
  src_y = VRAM_HEIGHT - src_y - height;
  dst_y = VRAM_HEIGHT - dst_y - height;

  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);
  glBlitFramebuffer(src_x, src_y, src_x + width, src_y + height, dst_x, dst_y, dst_x + width, dst_y + height,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);

  InvalidateVRAMReadCache();
}

void GPU_HW_OpenGL::UpdateVRAMReadTexture()
{
  m_stats.num_vram_read_texture_updates++;
  m_vram_read_texture_dirty = false;

  // TODO: Fallback blit path, and partial updates.
  glCopyImageSubData(m_framebuffer_texture->GetGLId(), GL_TEXTURE_2D, 0, 0, 0, 0, m_vram_read_texture->GetGLId(),
                     GL_TEXTURE_2D, 0, 0, 0, 0, VRAM_WIDTH, VRAM_HEIGHT, 1);
}

void GPU_HW_OpenGL::FlushRender()
{
  if (m_batch.vertices.empty())
    return;

  if (m_vram_read_texture_dirty)
    UpdateVRAMReadTexture();

  m_stats.num_batches++;
  m_stats.num_vertices += static_cast<u32>(m_batch.vertices.size());

  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  SetProgram();
  SetViewport();
  SetScissor();
  SetBlendState();

  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer_fbo_id);
  glBindVertexArray(m_vao_id);

  Assert((m_batch.vertices.size() * sizeof(HWVertex)) <= VERTEX_BUFFER_SIZE);
  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizei>(sizeof(HWVertex) * m_batch.vertices.size()),
                  m_batch.vertices.data());

  static constexpr std::array<GLenum, 3> gl_primitives = {{GL_LINES, GL_TRIANGLES, GL_TRIANGLE_STRIP}};
  glDrawArrays(gl_primitives[static_cast<u8>(m_batch.primitive)], 0, static_cast<GLsizei>(m_batch.vertices.size()));

  m_batch.vertices.clear();
}

std::unique_ptr<GPU> GPU::CreateHardwareOpenGLRenderer()
{
  return std::make_unique<GPU_HW_OpenGL>();
}
