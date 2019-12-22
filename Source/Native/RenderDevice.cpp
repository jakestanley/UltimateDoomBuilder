﻿
#include "Precomp.h"
#include "RenderDevice.h"
#include "VertexBuffer.h"
#include "IndexBuffer.h"
#include "Texture.h"
#include "ShaderManager.h"
#include <stdexcept>
#include <cstdarg>
#include <algorithm>
#include <cmath>

static bool GLLogStarted = false;
static void APIENTRY GLLogCallback(GLenum source, GLenum type, GLuint id,
	GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
{
	FILE* f = fopen("OpenGLDebug.log", GLLogStarted ? "a" : "w");
	if (!f) return;
	GLLogStarted = true;
	fprintf(f, "%s\r\n", message);
	fclose(f);
}

RenderDevice::RenderDevice(void* disp, void* window)
{
	Context = IOpenGLContext::Create(disp, window);
	if (Context)
	{
		Context->MakeCurrent();

#ifdef _DEBUG
		glEnable(GL_DEBUG_OUTPUT);
		glDebugMessageCallback(&GLLogCallback, nullptr);
#endif

		glGenVertexArrays(1, &mStreamVAO);
		glGenBuffers(1, &mStreamVertexBuffer);
		glBindVertexArray(mStreamVAO);
		glBindBuffer(GL_ARRAY_BUFFER, mStreamVertexBuffer);
		SharedVertexBuffer::SetupFlatVAO();

		int i = 0;
		for (auto& sharedbuf : mSharedVertexBuffers)
		{
			sharedbuf.reset(new SharedVertexBuffer((VertexFormat)i, (int64_t)16 * 1024 * 1024));
			glBindBuffer(GL_ARRAY_BUFFER, sharedbuf->GetBuffer());
			glBufferData(GL_ARRAY_BUFFER, sharedbuf->Size, nullptr, GL_STATIC_DRAW);
			i++;
		}

		glBindBuffer(GL_ARRAY_BUFFER, 0);

		mShaderManager = std::make_unique<ShaderManager>();

		CheckGLError();
	}
}

RenderDevice::~RenderDevice()
{
	if (Context)
	{
		Context->MakeCurrent();

		ProcessDeleteList();

		glDeleteBuffers(1, &mStreamVertexBuffer);
		glDeleteVertexArrays(1, &mStreamVAO);

		for (auto& sharedbuf : mSharedVertexBuffers)
		{
			for (VertexBuffer* buf : sharedbuf->VertexBuffers)
				buf->Device = nullptr;

			GLuint handle = sharedbuf->GetBuffer();
			glDeleteBuffers(1, &handle);
			handle = sharedbuf->GetVAO();
			glDeleteVertexArrays(1, &handle);
		}

		for (auto& it : mSamplers)
		{
			for (GLuint handle : it.second.WrapModes)
			{
				if (handle != 0)
					glDeleteSamplers(1, &handle);
			}
		}

		mShaderManager->ReleaseResources();
		Context->ClearCurrent();
	}
}

void RenderDevice::DeclareShader(ShaderName index, const char* name, const char* vertexshader, const char* fragmentshader)
{
	if (!mContextIsCurrent) Context->MakeCurrent();
	mShaderManager->DeclareShader(index, name, vertexshader, fragmentshader);
}

void RenderDevice::SetVertexBuffer(VertexBuffer* buffer)
{
	if (buffer != nullptr)
	{
		mVertexBufferStartIndex = buffer->BufferStartIndex;
		if (mVertexBuffer != (int)buffer->Format)
		{
			mVertexBuffer = (int)buffer->Format;
			mNeedApply = true;
			mVertexBufferChanged = true;
		}
	}
	else
	{
		mVertexBufferStartIndex = 0;
		if (mVertexBuffer != -1)
		{
			mVertexBuffer = -1;
			mNeedApply = true;
			mVertexBufferChanged = true;
		}
	}
}

void RenderDevice::SetIndexBuffer(IndexBuffer* buffer)
{
	if (mIndexBuffer != buffer)
	{
		mIndexBuffer = buffer;
		mNeedApply = true;
		mIndexBufferChanged = true;
	}
}

void RenderDevice::SetAlphaBlendEnable(bool value)
{
	if (mAlphaBlend != value)
	{
		mAlphaBlend = value;
		mNeedApply = true;
		mBlendStateChanged = true;
	}
}

void RenderDevice::SetAlphaTestEnable(bool value)
{
	if (mAlphaTest != value)
	{
		mAlphaTest = value;
		mNeedApply = true;
		mShaderChanged = true;
		mUniformsChanged = true;
	}
}

void RenderDevice::SetCullMode(Cull mode)
{
	if (mCullMode != mode)
	{
		mCullMode = mode;
		mNeedApply = true;
		mRasterizerStateChanged = true;
	}
}

void RenderDevice::SetBlendOperation(BlendOperation op)
{
	if (mBlendOperation != op)
	{
		mBlendOperation = op;
		mNeedApply = true;
		mBlendStateChanged = true;
	}
}

void RenderDevice::SetSourceBlend(Blend blend)
{
	if (mSourceBlend != blend)
	{
		mSourceBlend = blend;
		mNeedApply = true;
		mBlendStateChanged = true;
	}
}

void RenderDevice::SetDestinationBlend(Blend blend)
{
	if (mDestinationBlend != blend)
	{
		mDestinationBlend = blend;
		mNeedApply = true;
		mBlendStateChanged = true;
	}
}

void RenderDevice::SetFillMode(FillMode mode)
{
	if (mFillMode != mode)
	{
		mFillMode = mode;
		mNeedApply = true;
		mRasterizerStateChanged = true;
	}
}

void RenderDevice::SetMultisampleAntialias(bool value)
{
}

void RenderDevice::SetZEnable(bool value)
{
	if (mDepthTest != value)
	{
		mDepthTest = value;
		mNeedApply = true;
		mDepthStateChanged = true;
	}
}

void RenderDevice::SetZWriteEnable(bool value)
{
	if (mDepthWrite != value)
	{
		mDepthWrite = value;
		mNeedApply = true;
		mDepthStateChanged = true;
	}
}

void RenderDevice::SetTexture(Texture* texture)
{
	if (mTextureUnit.Tex != texture)
	{
		mTextureUnit.Tex = texture;
		mNeedApply = true;
		mTexturesChanged = true;
	}
}

void RenderDevice::SetSamplerFilter(TextureFilter minfilter, TextureFilter magfilter, TextureFilter mipfilter, float maxanisotropy)
{
	SamplerFilterKey key;
	key.MinFilter = GetGLMinFilter(minfilter, mipfilter);
	key.MagFilter = (magfilter == TextureFilter::Point || magfilter == TextureFilter::None) ? GL_NEAREST : GL_LINEAR;
	key.MaxAnisotropy = maxanisotropy;
	if (mSamplerFilterKey != key)
	{
		mSamplerFilterKey = key;
		mSamplerFilter = &mSamplers[mSamplerFilterKey];

		mNeedApply = true;
		mTexturesChanged = true;
	}
}

GLint RenderDevice::GetGLMinFilter(TextureFilter filter, TextureFilter mipfilter)
{
	if (mipfilter == TextureFilter::Linear)
	{
		if (filter == TextureFilter::Point || filter == TextureFilter::None)
			return GL_LINEAR_MIPMAP_NEAREST;
		else
			return GL_LINEAR_MIPMAP_LINEAR;
	}
	else if (mipfilter == TextureFilter::Point)
	{
		if (filter == TextureFilter::Point || filter == TextureFilter::None)
			return GL_NEAREST_MIPMAP_NEAREST;
		else
			return GL_NEAREST_MIPMAP_LINEAR;
	}
	else
	{
		if (filter == TextureFilter::Point || filter == TextureFilter::None)
			return GL_NEAREST;
		else
			return GL_LINEAR;
	}
}

void RenderDevice::SetSamplerState(TextureAddress address)
{
	if (mTextureUnit.WrapMode != address)
	{
		mTextureUnit.WrapMode = address;
		mNeedApply = true;
		mTexturesChanged = true;
	}
}

bool RenderDevice::Draw(PrimitiveType type, int startIndex, int primitiveCount)
{
	static const int modes[] = { GL_LINES, GL_TRIANGLES, GL_TRIANGLE_STRIP };
	static const int toVertexCount[] = { 2, 3, 1 };
	static const int toVertexStart[] = { 0, 0, 2 };

	if (mNeedApply && !ApplyChanges()) return false;
	glDrawArrays(modes[(int)type], mVertexBufferStartIndex + startIndex, toVertexStart[(int)type] + primitiveCount * toVertexCount[(int)type]);
	return CheckGLError();
}

bool RenderDevice::DrawIndexed(PrimitiveType type, int startIndex, int primitiveCount)
{
	static const int modes[] = { GL_LINES, GL_TRIANGLES, GL_TRIANGLE_STRIP };
	static const int toVertexCount[] = { 2, 3, 1 };
	static const int toVertexStart[] = { 0, 0, 2 };

	if (mNeedApply && !ApplyChanges()) return false;
	glDrawElementsBaseVertex(modes[(int)type], toVertexStart[(int)type] + primitiveCount * toVertexCount[(int)type], GL_UNSIGNED_INT, (const void*)(startIndex * sizeof(uint32_t)), mVertexBufferStartIndex);
	return CheckGLError();
}

bool RenderDevice::DrawData(PrimitiveType type, int startIndex, int primitiveCount, const void* data)
{
	static const int modes[] = { GL_LINES, GL_TRIANGLES, GL_TRIANGLE_STRIP };
	static const int toVertexCount[] = { 2, 3, 1 };
	static const int toVertexStart[] = { 0, 0, 2 };

	int vertcount = toVertexStart[(int)type] + primitiveCount * toVertexCount[(int)type];

	if (mNeedApply && !ApplyChanges()) return false;

	glBindBuffer(GL_ARRAY_BUFFER, mStreamVertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, vertcount * (size_t)SharedVertexBuffer::FlatStride, static_cast<const uint8_t*>(data) + startIndex * (size_t)SharedVertexBuffer::FlatStride, GL_STREAM_DRAW);
	glBindVertexArray(mStreamVAO);
	glDrawArrays(modes[(int)type], 0, vertcount);
	if (!CheckGLError()) return false;

	return ApplyVertexBuffer();
}

bool RenderDevice::StartRendering(bool clear, int backcolor, Texture* target, bool usedepthbuffer)
{
	Context->MakeCurrent();
	mContextIsCurrent = true;

	if (target)
	{
		GLuint framebuffer = 0;
		try
		{
			framebuffer = target->GetFramebuffer(this, usedepthbuffer);
		}
		catch (std::runtime_error& e)
		{
			SetError("Error setting render target: %s", e.what());
			return false;
		}
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
		mViewportWidth = target->GetWidth();
		mViewportHeight = target->GetHeight();
		if (!ApplyViewport()) return false;
	}
	else
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		mViewportWidth = Context->GetWidth();
		mViewportHeight = Context->GetHeight();
		if (!ApplyViewport()) return false;
	}

	if (clear && usedepthbuffer)
	{
		glEnable(GL_DEPTH_TEST);
		glDepthMask(GL_TRUE);
		glClearColor(RPART(backcolor) / 255.0f, GPART(backcolor) / 255.0f, BPART(backcolor) / 255.0f, APART(backcolor) / 255.0f);
		glClearDepthf(1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}
	else if (clear)
	{
		glClearColor(RPART(backcolor) / 255.0f, GPART(backcolor) / 255.0f, BPART(backcolor) / 255.0f, APART(backcolor) / 255.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	mNeedApply = true;
	mShaderChanged = true;
	mUniformsChanged = true;
	mTexturesChanged = true;
	mIndexBufferChanged = true;
	mVertexBufferChanged = true;
	mDepthStateChanged = true;
	mBlendStateChanged = true;
	mRasterizerStateChanged = true;

	return CheckGLError();
}

bool RenderDevice::FinishRendering()
{
	mContextIsCurrent = false;
	return true;
}

bool RenderDevice::Present()
{
	Context->MakeCurrent();
	Context->SwapBuffers();
	ProcessDeleteList();
	return CheckGLError();
}

bool RenderDevice::ClearTexture(int backcolor, Texture* texture)
{
	if (!StartRendering(true, backcolor, texture, false)) return false;
	return FinishRendering();
}

bool RenderDevice::CopyTexture(Texture* dst, CubeMapFace face)
{
	static const GLenum facegl[] = {
		GL_TEXTURE_CUBE_MAP_POSITIVE_X,
		GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
		GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_Z
	};

	if (!mContextIsCurrent) Context->MakeCurrent();
	GLint oldTexture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &oldTexture);

	glBindTexture(GL_TEXTURE_CUBE_MAP, dst->GetTexture(this));
	glCopyTexSubImage2D(facegl[(int)face], 0, 0, 0, 0, 0, dst->GetWidth(), dst->GetHeight());
	if (face == CubeMapFace::NegativeZ)
		glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

	glBindTexture(GL_TEXTURE_CUBE_MAP, oldTexture);
	bool result = CheckGLError();
	return result;
}

void RenderDevice::GarbageCollectBuffer(int size, VertexFormat format)
{
	auto& sharedbuf = mSharedVertexBuffers[(int)format];
	if (sharedbuf->NextPos + size <= sharedbuf->Size)
		return;

	GLint oldarray = 0, oldvao = 0;
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldarray);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldvao);

	int totalSize = size;
	for (VertexBuffer* buf : sharedbuf->VertexBuffers)
		totalSize += buf->Size;

	// If buffer is only half full we only need to GC. Otherwise we also need to expand the buffer size.
	int newSize = std::max(totalSize, sharedbuf->Size);
	if (newSize < totalSize * 2) newSize *= 2;

	std::unique_ptr<SharedVertexBuffer> old = std::move(sharedbuf);
	sharedbuf.reset(new SharedVertexBuffer(format, newSize));

	glBindBuffer(GL_ARRAY_BUFFER, sharedbuf->GetBuffer());
	glBufferData(GL_ARRAY_BUFFER, sharedbuf->Size, nullptr, GL_STATIC_DRAW);

	glBindBuffer(GL_COPY_READ_BUFFER, old->GetBuffer());

	// Copy all ranges still in use to the new buffer
	int stride = (format == VertexFormat::Flat ? SharedVertexBuffer::FlatStride : SharedVertexBuffer::WorldStride);
	int readPos = 0;
	int writePos = 0;
	int copySize = 0;
	for (VertexBuffer* buf : old->VertexBuffers)
	{
		if (buf->BufferOffset != readPos + copySize)
		{
			if (copySize != 0)
				glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_ARRAY_BUFFER, readPos, writePos, copySize);
			readPos = buf->BufferOffset;
			writePos += copySize;
			copySize = 0;
		}

		buf->BufferOffset = sharedbuf->NextPos;
		buf->BufferStartIndex = buf->BufferOffset / stride;
		sharedbuf->NextPos += buf->Size;
		copySize += buf->Size;
	}
	if (copySize != 0)
		glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_ARRAY_BUFFER, readPos, writePos, copySize);
	sharedbuf->VertexBuffers.swap(old->VertexBuffers);
	glBindBuffer(GL_COPY_READ_BUFFER, 0);

	GLuint handle = old->GetVAO();
	glDeleteVertexArrays(1, &handle);
	if (handle == oldvao) oldvao = sharedbuf->GetVAO();

	handle = old->GetBuffer();
	glDeleteBuffers(1, &handle);
	if (handle == oldarray) oldarray = sharedbuf->GetBuffer();

	glBindBuffer(GL_ARRAY_BUFFER, oldarray);
	glBindVertexArray(oldvao);

	mVertexBufferChanged = true;
	mNeedApply = true;
}

bool RenderDevice::SetVertexBufferData(VertexBuffer* buffer, void* data, int64_t size, VertexFormat format)
{
	if (!mContextIsCurrent) Context->MakeCurrent();

	if (buffer->Device)
	{
		buffer->Device->mSharedVertexBuffers[(int)buffer->Format]->VertexBuffers.erase(buffer->ListIt);
		buffer->Device = nullptr;
	}

	GarbageCollectBuffer(size, format);

	auto& sharedbuf = mSharedVertexBuffers[(int)format];

	GLint oldbinding = 0;
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldbinding);
	glBindBuffer(GL_ARRAY_BUFFER, sharedbuf->GetBuffer());

	buffer->ListIt = sharedbuf->VertexBuffers.insert(sharedbuf->VertexBuffers.end(), buffer);
	buffer->Device = this;
	buffer->Size = size;
	buffer->Format = format;
	buffer->BufferOffset = sharedbuf->NextPos;
	buffer->BufferStartIndex = buffer->BufferOffset / (format == VertexFormat::Flat ? SharedVertexBuffer::FlatStride : SharedVertexBuffer::WorldStride);
	sharedbuf->NextPos += size;

	glBufferSubData(GL_ARRAY_BUFFER, buffer->BufferOffset, size, data);
	glBindBuffer(GL_ARRAY_BUFFER, oldbinding);
	bool result = CheckGLError();
	return result;
}

bool RenderDevice::SetVertexBufferSubdata(VertexBuffer* buffer, int64_t destOffset, void* data, int64_t size)
{
	if (!mContextIsCurrent) Context->MakeCurrent();
	GLint oldbinding = 0;
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldbinding);
	glBindBuffer(GL_ARRAY_BUFFER, mSharedVertexBuffers[(int)buffer->Format]->GetBuffer());
	glBufferSubData(GL_ARRAY_BUFFER, buffer->BufferOffset + destOffset, size, data);
	glBindBuffer(GL_ARRAY_BUFFER, oldbinding);
	bool result = CheckGLError();
	return result;
}

bool RenderDevice::SetIndexBufferData(IndexBuffer* buffer, void* data, int64_t size)
{
	if (!mContextIsCurrent) Context->MakeCurrent();
	GLint oldbinding = 0;
	glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &oldbinding);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer->GetBuffer());
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, size, data, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, oldbinding);
	bool result = CheckGLError();
	return result;
}

bool RenderDevice::SetPixels(Texture* texture, const void* data)
{
	texture->SetPixels(data);
	return InvalidateTexture(texture);
}

bool RenderDevice::SetCubePixels(Texture* texture, CubeMapFace face, const void* data)
{
	texture->SetCubePixels(face, data);
	return InvalidateTexture(texture);
}

void* RenderDevice::MapPBO(Texture* texture)
{
	if (!mContextIsCurrent) Context->MakeCurrent();
	GLint pbo = texture->GetPBO(this);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
	void* buf = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
	bool result = CheckGLError();
	if (!result && buf)
	{
		glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
		buf = nullptr;
	}
	return buf;
}

bool RenderDevice::UnmapPBO(Texture* texture)
{
	if (!mContextIsCurrent) Context->MakeCurrent();
	GLint pbo = texture->GetPBO(this);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
	glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
	glBindTexture(GL_TEXTURE_2D, texture->GetTexture(this));
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texture->GetWidth(), texture->GetHeight(), 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
	bool result = CheckGLError();
	mNeedApply = true;
	mTexturesChanged = true;
	return result;
}

bool RenderDevice::InvalidateTexture(Texture* texture)
{
	if (texture->IsTextureCreated())
	{
		if (!mContextIsCurrent) Context->MakeCurrent();
		texture->Invalidate();
		bool result = CheckGLError();
		mNeedApply = true;
		mTexturesChanged = true;
		return result;
	}
	else
	{
		return true;
	}
}

bool RenderDevice::CheckGLError()
{
	if (!Context->IsCurrent())
	{
		SetError("Unexpected current OpenGL context");
	}

	GLenum error = glGetError();
	if (error == GL_NO_ERROR)
		return true;

	SetError("OpenGL error: %d", error);
	return false;
}

void RenderDevice::SetError(const char* fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	mSetErrorBuffer[0] = 0;
	mSetErrorBuffer[sizeof(mSetErrorBuffer) - 1] = 0;
	_vsnprintf(mSetErrorBuffer, sizeof(mSetErrorBuffer)-1, fmt, va);
	va_end(va);
	mLastError = mSetErrorBuffer;
}

const char* RenderDevice::GetError()
{
	mReturnError.swap(mLastError);
	mLastError.clear();
	return mReturnError.c_str();
}

Shader* RenderDevice::GetActiveShader()
{
	if (mAlphaTest)
		return &mShaderManager->AlphaTestShaders[(int)mShaderName];
	else
		return &mShaderManager->Shaders[(int)mShaderName];
}

void RenderDevice::SetShader(ShaderName name)
{
	if (name != mShaderName)
	{
		mShaderName = name;
		mNeedApply = true;
		mShaderChanged = true;
		mUniformsChanged = true;
	}
}

void RenderDevice::SetUniform(UniformName name, const void* values, int count)
{
	float* dest = mUniformData.data() + mUniformInfo[(int)name].Offset;
	if (memcmp(dest, values, sizeof(float) * count) != 0)
	{
		memcpy(dest, values, sizeof(float) * count);
		mUniformInfo[(int)name].LastUpdate++;
		mNeedApply = true;
		mUniformsChanged = true;
	}
}

bool RenderDevice::ApplyChanges()
{
	if (mShaderChanged && !ApplyShader()) return false;
	if (mVertexBufferChanged && !ApplyVertexBuffer()) return false;
	if (mIndexBufferChanged && !ApplyIndexBuffer()) return false;
	if (mUniformsChanged && !ApplyUniforms()) return false;
	if (mTexturesChanged && !ApplyTextures()) return false;
	if (mRasterizerStateChanged && !ApplyRasterizerState()) return false;
	if (mBlendStateChanged && !ApplyBlendState()) return false;
	if (mDepthStateChanged && !ApplyDepthState()) return false;

	mNeedApply = false;
	return true;
}

bool RenderDevice::ApplyViewport()
{
	glViewport(0, 0, mViewportWidth, mViewportHeight);
	return CheckGLError();
}

bool RenderDevice::ApplyShader()
{
	Shader* curShader = GetActiveShader();
	if (!curShader->CheckCompile(this))
	{
		SetError("Failed to bind shader:\r\n%s", curShader->GetCompileError().c_str());
		return false;
	}

	curShader->Bind();
	mShaderChanged = false;

	return CheckGLError();
}

bool RenderDevice::ApplyRasterizerState()
{
	if (mCullMode == Cull::None)
	{
		glDisable(GL_CULL_FACE);
	}
	else
	{
		glEnable(GL_CULL_FACE);
		glFrontFace(GL_CW);
	}

	GLenum fillMode2GL[] = { GL_FILL, GL_LINE };
	glPolygonMode(GL_FRONT_AND_BACK, fillMode2GL[(int)mFillMode]);

	mRasterizerStateChanged = false;

	return CheckGLError();
}

bool RenderDevice::ApplyBlendState()
{
	if (mAlphaBlend)
	{
		static const GLenum blendOp2GL[] = { GL_FUNC_ADD, GL_FUNC_REVERSE_SUBTRACT };
		static const GLenum blendFunc2GL[] = { GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE };

		glEnable(GL_BLEND);
		glBlendEquation(blendOp2GL[(int)mBlendOperation]);
		glBlendFunc(blendFunc2GL[(int)mSourceBlend], blendFunc2GL[(int)mDestinationBlend]);
	}
	else
	{
		glDisable(GL_BLEND);
	}

	mBlendStateChanged = false;

	return CheckGLError();
}

bool RenderDevice::ApplyDepthState()
{
	if (mDepthTest)
	{
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LEQUAL);
		glDepthMask(mDepthWrite ? GL_TRUE : GL_FALSE);
	}
	else
	{
		glDisable(GL_DEPTH_TEST);
	}

	mDepthStateChanged = false;

	return CheckGLError();
}

bool RenderDevice::ApplyIndexBuffer()
{
	if (mIndexBuffer)
	{
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIndexBuffer->GetBuffer());
	}
	else
	{
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}

	mIndexBufferChanged = false;

	return CheckGLError();
}

bool RenderDevice::ApplyVertexBuffer()
{
	if (mVertexBuffer != -1)
		glBindVertexArray(mSharedVertexBuffers[mVertexBuffer]->GetVAO());

	mVertexBufferChanged = false;

	return CheckGLError();
}

void RenderDevice::DeclareUniform(UniformName name, const char* glslname, UniformType type)
{
	size_t index = (size_t)name;
	if (mUniformInfo.size() <= index)
		mUniformInfo.resize(index + 1);

	UniformInfo& info = mUniformInfo[index];
	info.Name = glslname;
	info.Type = type;
	info.Offset = (int)mUniformData.size();

	mUniformData.resize(mUniformData.size() + (type == UniformType::Mat4 ? 16 : 4));
}

bool RenderDevice::ApplyUniforms()
{
	Shader* shader = GetActiveShader();
	GLuint* locations = shader->UniformLocations.data();
	int* lastupdates = shader->UniformLastUpdates.data();

	int count = (int)mUniformInfo.size();
	for (int i = 0; i < count; i++)
	{
		if (lastupdates[i] != mUniformInfo.data()[i].LastUpdate)
		{
			float* data = mUniformData.data() + mUniformInfo[i].Offset;
			GLuint location = locations[i];
			switch (mUniformInfo[i].Type)
			{
			default:
			case UniformType::Vec4f: glUniform4fv(location, 1, data); break;
			case UniformType::Vec3f: glUniform3fv(location, 1, data); break;
			case UniformType::Vec2f: glUniform2fv(location, 1, data); break;
			case UniformType::Float: glUniform1fv(location, 1, data); break;
			case UniformType::Mat4: glUniformMatrix4fv(location, 1, GL_FALSE, data); break;
			}
			lastupdates[i] = mUniformInfo[i].LastUpdate;
		}
	}

	mUniformsChanged = false;

	return CheckGLError();
}

bool RenderDevice::ApplyTextures()
{
	glActiveTexture(GL_TEXTURE0);
	if (mTextureUnit.Tex)
	{
		GLenum target = mTextureUnit.Tex->IsCubeTexture() ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;

		glBindTexture(target, mTextureUnit.Tex->GetTexture(this));

		GLuint& samplerHandle = mSamplerFilter->WrapModes[(int)mTextureUnit.WrapMode];
		if (samplerHandle == 0)
		{
			static const int wrapMode[] = { GL_REPEAT, GL_CLAMP_TO_EDGE };

			glGenSamplers(1, &samplerHandle);
			glSamplerParameteri(samplerHandle, GL_TEXTURE_MIN_FILTER, mSamplerFilterKey.MinFilter);
			glSamplerParameteri(samplerHandle, GL_TEXTURE_MAG_FILTER, mSamplerFilterKey.MagFilter);
			glSamplerParameteri(samplerHandle, GL_TEXTURE_WRAP_S, wrapMode[(int)mTextureUnit.WrapMode]);
			glSamplerParameteri(samplerHandle, GL_TEXTURE_WRAP_T, wrapMode[(int)mTextureUnit.WrapMode]);
			glSamplerParameteri(samplerHandle, GL_TEXTURE_WRAP_R, wrapMode[(int)mTextureUnit.WrapMode]);
		}

		if (mTextureUnit.SamplerHandle != samplerHandle)
		{
			mTextureUnit.SamplerHandle = samplerHandle;
			glBindSampler(0, samplerHandle);
		}
	}
	else
	{
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	mTexturesChanged = false;

	return CheckGLError();
}

std::mutex& RenderDevice::GetMutex()
{
	static std::mutex m;
	return m;
}

void RenderDevice::DeleteObject(VertexBuffer* buffer)
{
	std::unique_lock<std::mutex> lock(RenderDevice::GetMutex());
	if (buffer->Device)
		buffer->Device->mDeleteList.VertexBuffers.push_back(buffer);
	else
		delete buffer;
}

void RenderDevice::DeleteObject(IndexBuffer* buffer)
{
	std::unique_lock<std::mutex> lock(RenderDevice::GetMutex());
	if (buffer->Device)
		buffer->Device->mDeleteList.IndexBuffers.push_back(buffer);
	else
		delete buffer;
}

void RenderDevice::DeleteObject(Texture* texture)
{
	std::unique_lock<std::mutex> lock(RenderDevice::GetMutex());
	if (texture->Device)
		texture->Device->mDeleteList.Textures.push_back(texture);
	else
		delete texture;
}

void RenderDevice::ProcessDeleteList()
{
	std::unique_lock<std::mutex> lock(RenderDevice::GetMutex());
	for (auto buffer : mDeleteList.IndexBuffers) delete buffer;
	for (auto buffer : mDeleteList.VertexBuffers) delete buffer;
	for (auto texture : mDeleteList.Textures) delete texture;
	mDeleteList.IndexBuffers.clear();
	mDeleteList.VertexBuffers.clear();
	mDeleteList.Textures.clear();
}

/////////////////////////////////////////////////////////////////////////////

extern "C"
{

RenderDevice* RenderDevice_New(void* disp, void* window)
{
	RenderDevice *device = new RenderDevice(disp, window);
	if (!device->Context)
	{
		delete device;
		return nullptr;
	}
	else
	{
		return device;
	}
}

void RenderDevice_Delete(RenderDevice* device)
{
	delete device;
}

const char* RenderDevice_GetError(RenderDevice* device)
{
	return device->GetError();
}

void RenderDevice_DeclareUniform(RenderDevice* device, UniformName name, const char* variablename, UniformType type)
{
	device->DeclareUniform(name, variablename, type);
}

void RenderDevice_DeclareShader(RenderDevice* device, ShaderName index, const char* name, const char* vertexshader, const char* fragmentshader)
{
	device->DeclareShader(index, name, vertexshader, fragmentshader);
}

void RenderDevice_SetShader(RenderDevice* device, ShaderName name)
{
	device->SetShader(name);
}

void RenderDevice_SetUniform(RenderDevice* device, UniformName name, const void* values, int count)
{
	device->SetUniform(name, values, count);
}

void RenderDevice_SetVertexBuffer(RenderDevice* device, VertexBuffer* buffer)
{
	device->SetVertexBuffer(buffer);
}

void RenderDevice_SetIndexBuffer(RenderDevice* device, IndexBuffer* buffer)
{
	device->SetIndexBuffer(buffer);
}

void RenderDevice_SetAlphaBlendEnable(RenderDevice* device, bool value)
{
	device->SetAlphaBlendEnable(value);
}

void RenderDevice_SetAlphaTestEnable(RenderDevice* device, bool value)
{
	device->SetAlphaTestEnable(value);
}

void RenderDevice_SetCullMode(RenderDevice* device, Cull mode)
{
	device->SetCullMode(mode);
}

void RenderDevice_SetBlendOperation(RenderDevice* device, BlendOperation op)
{
	device->SetBlendOperation(op);
}

void RenderDevice_SetSourceBlend(RenderDevice* device, Blend blend)
{
	device->SetSourceBlend(blend);
}

void RenderDevice_SetDestinationBlend(RenderDevice* device, Blend blend)
{
	device->SetDestinationBlend(blend);
}

void RenderDevice_SetFillMode(RenderDevice* device, FillMode mode)
{
	device->SetFillMode(mode);
}

void RenderDevice_SetMultisampleAntialias(RenderDevice* device, bool value)
{
	device->SetMultisampleAntialias(value);
}

void RenderDevice_SetZEnable(RenderDevice* device, bool value)
{
	device->SetZEnable(value);
}

void RenderDevice_SetZWriteEnable(RenderDevice* device, bool value)
{
	device->SetZWriteEnable(value);
}

void RenderDevice_SetTexture(RenderDevice* device, Texture* texture)
{
	device->SetTexture(texture);
}

void RenderDevice_SetSamplerFilter(RenderDevice* device, TextureFilter minfilter, TextureFilter magfilter, TextureFilter mipfilter, float maxanisotropy)
{
	device->SetSamplerFilter(minfilter, magfilter, mipfilter, maxanisotropy);
}

void RenderDevice_SetSamplerState(RenderDevice* device, TextureAddress address)
{
	device->SetSamplerState(address);
}

bool RenderDevice_Draw(RenderDevice* device, PrimitiveType type, int startIndex, int primitiveCount)
{
	return device->Draw(type, startIndex, primitiveCount);
}

bool RenderDevice_DrawIndexed(RenderDevice* device, PrimitiveType type, int startIndex, int primitiveCount)
{
	return device->DrawIndexed(type, startIndex, primitiveCount);
}

bool RenderDevice_DrawData(RenderDevice* device, PrimitiveType type, int startIndex, int primitiveCount, const void* data)
{
	return device->DrawData(type, startIndex, primitiveCount, data);
}

bool RenderDevice_StartRendering(RenderDevice* device, bool clear, int backcolor, Texture* target, bool usedepthbuffer)
{
	return device->StartRendering(clear, backcolor, target, usedepthbuffer);
}

bool RenderDevice_FinishRendering(RenderDevice* device)
{
	return device->FinishRendering();
}

bool RenderDevice_Present(RenderDevice* device)
{
	return device->Present();
}

bool RenderDevice_ClearTexture(RenderDevice* device, int backcolor, Texture* texture)
{
	return device->ClearTexture(backcolor, texture);
}

bool RenderDevice_CopyTexture(RenderDevice* device, Texture* dst, CubeMapFace face)
{
	return device->CopyTexture(dst, face);
}

bool RenderDevice_SetVertexBufferData(RenderDevice* device, VertexBuffer* buffer, void* data, int64_t size, VertexFormat format)
{
	return device->SetVertexBufferData(buffer, data, size, format);
}

bool RenderDevice_SetVertexBufferSubdata(RenderDevice* device, VertexBuffer* buffer, int64_t destOffset, void* data, int64_t size)
{
	return device->SetVertexBufferSubdata(buffer, destOffset, data, size);
}

bool RenderDevice_SetIndexBufferData(RenderDevice* device, IndexBuffer* buffer, void* data, int64_t size)
{
	return device->SetIndexBufferData(buffer, data, size);
}

bool RenderDevice_SetPixels(RenderDevice* device, Texture* texture, const void* data)
{
	return device->SetPixels(texture, data);
}

bool RenderDevice_SetCubePixels(RenderDevice* device, Texture* texture, CubeMapFace face, const void* data)
{
	return device->SetCubePixels(texture, face, data);
}

void* RenderDevice_MapPBO(RenderDevice* device, Texture* texture)
{
	return device->MapPBO(texture);
}

bool RenderDevice_UnmapPBO(RenderDevice* device, Texture* texture)
{
	return device->UnmapPBO(texture);
}

}
