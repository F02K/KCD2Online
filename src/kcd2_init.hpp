#pragma once

#include "engine_console_queue.hpp"

#include <config/config.hpp>
#include <ImGuizmo.h>
#include <input/hotkey.hpp>

namespace big
{
	struct table_patch_info_t
	{
		std::string m_mod_name;
		std::vector<uint8_t> m_patched_data;
	};

	inline std::map<std::string, std::map<std::string, std::vector<table_patch_info_t>>> g_table_name_to_added_line_to_info;
	inline std::map<std::string, std::map<std::string, std::vector<uint8_t>>> g_table_name_to_modified_line_to_original_data;
	inline std::map<std::string, std::map<std::string, std::vector<table_patch_info_t>>> g_table_name_to_modified_line_to_info;

	struct vanilla_mod_system_info
	{
		std::string m_name;
		std::string m_mod_id;
		std::string m_description;
		std::string m_author;
		std::string m_version;
		std::string m_folder_name;
		std::string m_created_on;
		std::vector<std::string> m_loaded_paks;
	};

	struct cry_cvar
	{
		virtual ~cry_cvar() = default;
		virtual void Release() = 0;
		virtual int GetIVal() const = 0;
		virtual std::int64_t GetI64Val() const = 0;
		virtual float GetFVal() const = 0;
		virtual const char *GetString() const = 0;
		virtual void Set(const char *value) = 0;
		virtual void Set(int value) = 0;
		virtual void Set(float value) = 0;
		virtual void Set(std::int64_t value) = 0;
		virtual void ClearFlags(int flags) = 0;
		virtual int GetFlags() const = 0;
		virtual int SetFlags(int flags) = 0;
		virtual int GetRealIVal() const = 0;
		virtual int GetType() const = 0;
		virtual const char *GetName() const = 0;
		virtual const char *GetHelpText() const = 0;
	};

	inline std::map<std::string, cry_cvar *> g_cvars;

	[[nodiscard]] bool engine_console_available();
	[[nodiscard]] bool engine_console_has_command(std::string_view name);
	[[nodiscard]] bool engine_cvar_available(std::string_view name);
	[[nodiscard]] std::optional<int> engine_cvar_int(std::string_view name);
	[[nodiscard]] std::optional<std::string> engine_cvar_string(
	    std::string_view name);
	[[nodiscard]] bool engine_cvar_set_int(std::string_view name, int value);
	[[nodiscard]] bool engine_console_execute(
	    std::string_view command,
	    bool defer_execution = true);
	[[nodiscard]] engine_console_submit_status queue_engine_console_command(
	    std::string command);
	inline std::vector<vanilla_mod_system_info> g_vanilla_mods;

	// global|level_name_specific -> xml filename
	inline ankerl::unordered_dense::map<std::string, ankerl::unordered_dense::set<std::string>> g_xml_context_to_modded_xml_filenames;

	// global|level_name_specific -> xml filename -> modifications buffer
	inline ankerl::unordered_dense::map<std::string, ankerl::unordered_dense::map<std::string, std::vector<std::string>>> g_xml_context_to_xml_filename_to_modifications;

	void apply_xml_patches(std::string &originalFileContent, const std::vector<std::string> &patchFileContents, bool is_inventory_preset);

	struct Vec3
	{
		float x, y, z;

		Vec3 operator*(float scalar) const
		{
			return {x * scalar, y * scalar, z * scalar};
		}

		Vec3 operator+(const Vec3 &other) const
		{
			return {x + other.x, y + other.y, z + other.z};
		}

		Vec3 operator-(const Vec3 &other) const
		{
			return {x - other.x, y - other.y, z - other.z};
		}

		Vec3 &operator+=(const Vec3 &other)
		{
			x += other.x;
			y += other.y;
			z += other.z;
			return *this;
		}

		Vec3 &operator-=(const Vec3 &other)
		{
			x -= other.x;
			y -= other.y;
			z -= other.z;
			return *this;
		}

		Vec3 Cross(const Vec3 &other) const
		{
			return {y * other.z - z * other.y, z * other.x - x * other.z, x * other.y - y * other.x};
		}

		float Length() const
		{
			return std::sqrt(x * x + y * y + z * z);
		}

		Vec3 Normalized() const
		{
			float len = Length();
			return (len > 0) ? (*this * (1.0f / len)) : Vec3{0, 0, 0};
		}

		std::string to_string()
		{
			std::ostringstream oss;
			oss << "Vec3(" << x << ", " << y << ", " << z << ")";
			return oss.str();
		}
	};

	inline bool operator==(const Vec3 &lhs, const Vec3 &rhs)
	{
		return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
	}

	inline bool operator!=(const Vec3 &lhs, const Vec3 &rhs)
	{
		return !(lhs == rhs);
	}

	struct AABB
	{
		Vec3 min;
		Vec3 max;
	};

	inline bool operator==(const AABB &lhs, const AABB &rhs)
	{
		return lhs.min == rhs.min && lhs.max == rhs.max;
	}

	inline bool operator!=(const AABB &lhs, const AABB &rhs)
	{
		return !(lhs == rhs);
	}

	std::pair<Vec3, Vec3> get_camera_position_and_direction();

	std::pair<float *, float *> get_camera_view_and_projection_matrix();

	struct Archetype
	{
		virtual ~Archetype() = default; // offset 0

		uint64_t unk_8;
		uint64_t unk_16;
		const char *m_name;
	};

	struct EntityClass
	{
		virtual ~EntityClass()        = default; // offset 0
		virtual void Pad1()           = 0;       // offset 8
		virtual const char *GetName() = 0;       // offset 16
	};

	struct CEntity;
	inline CEntity *g_player_entity                                           = nullptr;
	inline void *(*g_CEntity_SetWorldTM)(CEntity *this_, void *tm, int flags) = nullptr;

	enum EERType
	{
		eERType_NotRenderNode,
		eERType_Brush,
		eERType_Vegetation,
		eERType_Light,
		eERType_Dummy_5, //!< Used to be eERType_Cloud, preserve order for compatibility.
		eERType_MergedMeshInstance,
		eERType_FogVolume,
		eERType_Decal,
		eERType_ParticleEmitter,
		eERType_WaterVolume,
		eERType_WaterWave,
		eERType_Road,
		eERType_DistanceCloud,
		eERType_Dummy_4, // Used to be eERType_VolumeObject, preserve order for compatibility.
		eERType_Dummy_0, //!< Used to be eERType_AutoCubeMap, preserve order for compatibility.
		eERType_Rope,
		eERType_Dummy_3, //!< Used to be  eERType_PrismObject, preserve order for compatibility.
		eERType_TerrainSector,
		eERType_Dummy_2, //!< Used to be eERType_LightPropagationVolume, preserve order for compatibility.
		eERType_MovableBrush,
		eERType_GameEffect,
		eERType_BreakableGlass,
		eERType_CloudBlocker,
		eERType_MergedMesh,
		eERType_GeomCache,
		eERType_Character,
		eERType_TypesNum, //!< Must be at the end - gives the total number of ER types.
	};

	class CLodValue
	{
	public:
		CLodValue()
		{
			m_nLodA        = -1;
			m_nLodB        = -1;
			m_nDissolveRef = 0;
		}

		CLodValue(int nLodA)
		{
			m_nLodA        = nLodA;
			m_nLodB        = -1;
			m_nDissolveRef = 0;
		}

		CLodValue(int nLodA, uint8_t nDissolveRef, int nLodB)
		{
			m_nLodA        = nLodA;
			m_nLodB        = nLodB;
			m_nDissolveRef = nDissolveRef;
		}

		int LodA() const
		{
			return m_nLodA;
		}

		int LodSafeA() const
		{
			return m_nLodA == -1 ? 0 : m_nLodA;
		}

		int LodB() const
		{
			return m_nLodB;
		}

		unsigned int DissolveRefA() const
		{
			return m_nDissolveRef;
		}

		unsigned int DissolveRefB() const
		{
			return 255 - m_nDissolveRef;
		}

	private:
		int16_t m_nLodA;
		int16_t m_nLodB;
		uint8_t m_nDissolveRef;
	};

	const int MAX_GSM_CACHED_LODS_NUM = 3;
	struct IRenderNone;

	struct IShadowCaster
	{
		// 0
		virtual ~IShadowCaster() = 0;

		// 1
		virtual bool HasOcclusionmap(int nLod, void *pLightOwner) = 0;

		// 2
		virtual CLodValue ComputeLod(int wantedLod, const void *passInfo) = 0;

		// 3
		virtual void Render(const void *RendParams, const void *passInfo) = 0;

		// 4
		virtual const AABB GetBBox() const = 0;

		// 5
		virtual void FillBBox(AABB &aabb) const = 0;

		// 6
		virtual struct ICharacterInstance *GetEntityCharacter(void *pMatrix = NULL, bool bReturnOnlyVisible = false) = 0;

		// 7
		virtual EERType GetRenderNodeType() const = 0;

		void *m_unk;
	};

#define BIT8(x)  ((static_cast<uint8_t>(1)) << (x))
#define BIT32(x) ((static_cast<uint32_t>(1)) << (x))

	struct IRenderNode : public IShadowCaster
	{
		enum EInternalFlags : uint8_t
		{
			DECAL_OWNER                = BIT8(0), //!< Owns some decals.
			REQUIRES_NEAREST_CUBEMAP   = BIT8(1), //!< Pick nearest cube map.
			UPDATE_DECALS              = BIT8(2), //!< The node changed geometry - decals must be updated.
			REQUIRES_FORWARD_RENDERING = BIT8(3), //!< Special shadow processing needed.
			WAS_INVISIBLE              = BIT8(4), //!< Was invisible last frame.
			WAS_IN_VISAREA             = BIT8(5), //!< Was inside vis-ares last frame.
			WAS_FARAWAY         = BIT8(6), //!< Was considered 'far away' for the purposes of physics deactivation.
			HAS_OCCLUSION_PROXY = BIT8(7), //!< This node has occlusion proxy.
		};

		typedef uint64_t RenderFlagsType;

		struct SUpdateStreamingPriorityContext
		{
			const void *pPassInfo = nullptr;
			float distance        = 0.0f;
			float importance      = 0.0f;
			bool bFullUpdate      = false;
			int lod               = 0;
		};

	public:

		IRenderNode()
		{
			m_dwRndFlags           = 0;
			m_ucViewDistRatio      = 100;
			m_ucLodRatio           = 100;
			m_pOcNode              = nullptr;
			m_nHUDSilhouettesParam = 0;
			m_fWSMaxViewDist       = 0;
			m_nInternalFlags       = 0;
			m_nMaterialLayers      = 0;
			m_pTempData            = nullptr;
			m_pPrev = m_pNext = nullptr;

			//ZeroArray(m_shadowCacheLod);
			//ZeroArray(m_shadowCacheLastRendered);

			m_onePassTraversalFrameId        = 0;
			m_onePassTraversalShadowCascades = 0;
		}

		// 8
		virtual bool CanExecuteRenderAsJob() const = 0;

		// Debug info about object.
		// 9
		virtual const char *GetName() const = 0;

		// 10
		virtual const char *GetEntityClassName() const = 0;

		// 11
		virtual std::string GetDebugString(char type = 0) const = 0;

		// 12
		virtual float GetImportance() const = 0;

		//! Releases IRenderNode.
		// 13
		virtual void ReleaseNode(bool bImmediate = false) = 0;

		// 14
		virtual IRenderNode *Clone() const = 0;

		// TODO: Investigate what is that function.
		virtual void *Pad15() const = 0;

		//! Sets render node transformation matrix.
		// 16
		virtual void SetMatrix(const void *mat34) = 0;

		//! Gets local bounds of the render node.
		// 17
		virtual void GetLocalBounds(AABB &bbox) const = 0;

		// 18
		virtual Vec3 GetPos(bool bWorldOnly = true) const = 0;

		// 19
		virtual void SetBBox(const AABB &WSBBox) = 0;

		//! Changes the world coordinates position of this node by delta.
		//! Don't forget to call this base function when overriding it.
		// 20
		virtual void OffsetPosition(const Vec3 &delta) = 0;

		//! Is node geometry visible in passInfo's camera
		// 21
		virtual bool IsVisible(const AABB &nodeBox, const float nodeDistance, const void *passInfo) const = 0;

		//! Gives access to object components.
		virtual void *GetEntityStatObj(unsigned int nSubPartId = 0, void *pMatrix34a = NULL, bool bReturnOnlyVisible = false) = 0;

		virtual void *Pad23() const = 0;

		virtual void SetEntityStatObj(void *pStatObj, const void *pMatrix34a = NULL) = 0;

		virtual void *Pad25() const = 0;
		virtual void *Pad26() const = 0;

		//! \return IRenderMesh of the object.
		// 27
		virtual void *GetRenderMesh(int nLod) const = 0;

		//! Allows to adjust default lod distance settings.
		//! If fLodRatio is 100 - default lod distance is used.
		// 28
		virtual void SetLodRatio(int nLodRatio) = 0;

		//! Get material layers mask.
		// 29
		virtual uint8_t GetMaterialLayers() const = 0;

		//! Get physical entity.
		virtual struct IPhysicalEntity *GetPhysics() const = 0;
		virtual void SetPhysics(IPhysicalEntity *pPhys)    = 0;

		//! Physicalizes if it isn't already.
		virtual void CheckPhysicalized() = 0;

		//! Physicalize the node.
		virtual void Physicalize(bool bInstant = false) = 0;

		//! Physicalize stat object's foliage.
		virtual bool PhysicalizeFoliage(bool bPhysicalize = true, int iSource = 0, int nSlot = 0) = 0;

		//! Get physical entity (rope) for a given branch (if foliage is physicalized).
		virtual IPhysicalEntity *GetBranchPhys(int idx, int nSlot = 0) = 0;

		//! \return Physicalized foliage, or NULL if it isn't physicalized.
		//virtual struct IFoliage *GetFoliage(int nSlot = 0) = 0;
		virtual void *GetFoliage(int nSlot = 0) = 0;

		//! Set override material for this instance.
		virtual void SetMaterial(void *pMat) = 0;

		//! Queries override material of this instance.
		virtual void *GetMaterial(Vec3 *pHitPos = NULL) const = 0;
		virtual void *GetMaterialOverride() const             = 0;

		//! Used by the editor during export.
		virtual void SetCollisionClassIndex(int tableIndex) = 0;

		virtual void SetStatObjGroupIndex(int nVegetationGroupIndex) = 0;

		virtual int GetStatObjGroupId() const = 0;

		virtual void SetLayerId(uint16_t nLayerId) = 0;

		virtual uint16_t GetLayerId() const = 0;

		virtual float GetMaxViewDist() const = 0;

		virtual bool IsAllocatedOutsideOf3DEngineDLL() = 0;

		virtual void Dephysicalize(bool bKeepIfReferenced = false) = 0;

		virtual void Dematerialize() = 0;

		virtual void GetMemoryUsage(void *pSizer) const = 0;

		virtual void Precache() = 0;

		virtual void UpdateStreamingPriority(const SUpdateStreamingPriorityContext &streamingContext) = 0;

		//	virtual float GetLodForDistance(float fDistance) { return 0; }

		//! Called immediately when render node becomes visible from any thread.
		//! Not reentrant, multiple simultaneous calls to this method on the same rendernode from multiple threads is not supported and should not happen
		virtual void OnRenderNodeBecomeVisibleAsync(void *pTempData, const void *passInfo) = 0;

		//! Called when RenderNode becomes visible or invisible, can only be called from the Main thread
		virtual void OnRenderNodeVisible(bool bBecomeVisible) = 0;

		virtual uint8_t GetSortPriority() const = 0;

		//! Object can be used by GI system in several ways.
		enum EGIMode
		{
			eGM_None = 0,             //!< No voxelization.
			eGM_StaticVoxelization,   //!< Incremental or asynchronous lazy voxelization.
			eGM_DynamicVoxelization,  //!< Real-time every-frame voxelization on GPU.
			eGM_HideIfGiIsActive,     //!< Hide this light source if GI is enabled
			eGM_AnalyticalProxy_Soft, //!< Analytical proxy (with shadow fading)
			eGM_AnalyticalProxy_Hard, //!< Analytical proxy (no shadow fading)
			eGM_AnalytPostOccluder,   //!< Analytical occluder (used with average light direction)
			eGM_IntegrateIntoTerrain, //!< Copy object mesh into terrain mesh and render using usual terrain materials
		};

		//! Retrieves the way object is used by GI system.
		virtual EGIMode GetGIMode() const = 0;

		virtual void SetMinSpec(RenderFlagsType nMinSpec) = 0;

		//! Allows to adjust default max view distance settings.
		//! If fMaxViewDistRatio is 100 - default max view distance is used.
		virtual void SetViewDistRatio(int nViewDistRatio) = 0;

		virtual bool GetLodDistances(const void *frameLodInfo, float *distances) const = 0;

		//! Bias value to add to the regular lod
		virtual void SetShadowLodBias(int8_t nShadowLodBias) = 0;

		//! Sets supplemental parameters used for camera-space rendering (ERF_FOB_NEAREST).
		//! Parameter values shall reflect the current render node position, as supplied with SetMatrix().
		//! Usage of this feature is optional. The benefit is increased rendering precision.
		virtual void SetCameraSpaceParams(void *cameraSpaceParams) = 0;

		//! Retrieves camera space parameters that have been previously set by a call to SetCameraSpaceParams().
		virtual void *GetCameraSpaceParams() const = 0;

		virtual void SetEditorObjectId(uint32_t nEditorObjectId) = 0;

		virtual void SetEditorObjectInfo(bool bSelected, bool bHighlighted) = 0;

		// Set a new owner entity
		virtual void SetOwnerEntity(void *pEntity) = 0;

		// Retrieve a pointer to the entity who owns this render node.
		virtual void *GetOwnerEntity() const = 0;

	private:
		template<class T>
		friend struct TDoublyLinkedList;
		friend struct IOctreeNode;
		friend class COctreeNode;

		//! Every sector has linked list of IRenderNode objects.
		IRenderNode *m_pNext, *m_pPrev;

	protected:

		//! Current objects tree cell.
		IOctreeNode *m_pOcNode;

	public:
		//! Render flags (@see ERenderNodeFlags)
		RenderFlagsType m_dwRndFlags;

	protected:

		//! Hides/shows node in renderer.
		virtual void Hide(bool bHide) = 0;

		//! Enables/disables instancing on the node
		virtual void Instance(bool bInstance) = 0;

	public:
		//! Pointer to temporary data allocated only for currently visible objects.
		void *m_pTempData       = nullptr;
		int m_manipulationFrame = -1;

		//! Hud silhouette parameter, default is black with alpha zero
		uint32_t m_nHUDSilhouettesParam;

		//! Used to request visiting of the node during one-pass traversal
		union
		{
			struct
			{
				uint32_t m_onePassTraversalFrameId;
				uint32_t m_onePassTraversalShadowCascades;
			};
		};

		//! Max view distance.
		float m_fWSMaxViewDist;

		//! Flags for render node internal usage, one or more bits from EInternalFlags.
		uint8_t m_nInternalFlags;

		//! Max view distance settings.
		uint8_t m_ucViewDistRatio;

		//! LOD settings.
		uint8_t m_ucLodRatio;

		//! Material layers bitmask -> which material layers are active.
		uint8_t m_nMaterialLayers;
	}; // namespace big

	struct IBrush : public IRenderNode
	{
		virtual const void *GetMatrix34() const            = 0;
		virtual void SetDrawLast(bool enable)              = 0;
		virtual void DisablePhysicalization(bool bDisable) = 0; //!< This render node will not be physicalized
		virtual float GetScale() const                     = 0;

		// Hide mask disable individual sub-objects rendering in the compound static objects
		// Only implemented by few nodes.
		//virtual void SetSubObjectHideMask(hidemask subObjHideMask);
		virtual void SetSubObjectHideMask(uint64_t subObjHideMask) = 0;
	};

	struct Cry3DEngineBase
	{
	};

	class CBrush : public IBrush, public Cry3DEngineBase
	{
		friend class COctreeNode;

	public:
		//CBrush();
		virtual ~CBrush() = 0;

		//inline Vec3 GetPos()
		//{
		//return {m_Matrix[3], m_Matrix[7], m_Matrix[11]};
		//}

		virtual bool HasChanged() = 0;

		const void *GetMatrix34() const
		{
			return m_Matrix;
		}

		bool GetDrawLast() const
		{
			return m_bDrawLast;
		}

		bool HasDeformableData() const
		{
			return m_pDeform != NULL;
		}

	private:

	public:
		// Transformation Matrix
		float m_Matrix[3 * 4];

		// Physical Entity, when this node is physicalized.
		IPhysicalEntity *m_pPhysEnt = nullptr;

		//! Override material, will override default material assigned to the Geometry.
		//_smart_ptr<IMaterial> m_pMaterial;
		void *m_pMaterial;

		uint16_t m_collisionClassIdx = 0;
		uint16_t m_nLayerId          = 0;

		// Geometry referenced and rendered by this node.
		//_smart_ptr<CStatObj> m_pStatObj;
		void *m_pStatObj;

		//CDeformableNode *m_pDeform = nullptr;
		void *m_pDeform = nullptr;
		//IFoliage *m_pFoliage = nullptr;
		void *m_pFoliage = nullptr;

		uint32_t m_bVehicleOnlyPhysics : 1;
		uint32_t m_bDrawLast           : 1;
		uint32_t m_bNoPhysicalize      : 1;

		// World space bounding box for this node.
		AABB m_WSBBox;

		// Last frame this object moved
		uint32_t m_lastMoveFrameId = 0;
		// Hide mask disable individual sub-objects rendering in the compound static objects
		//hidemask m_nSubObjHideMask;
		uint64_t m_nSubObjHideMask;

		uint64_t m_unk2;
		uint64_t m_unk3;
		uint64_t m_unk4;
		uint64_t m_unk5;
		uint64_t m_unk6;
	};

	inline void (*g_CBrush_SetStatObj)(CBrush *this_, void *new_stat_obj) = nullptr;

	inline void (*g_CStatObj_ctor)(uintptr_t this_) = nullptr;

	//! Each entity instance holds flags that indicate special behavior within the system
	//! These can be set with IEntity::SetFlags, and retrieved with IEntity::GetFlags
	enum EEntityFlags : uint32_t
	{
		//////////////////////////////////////////////////////////////////////////
		// Persistent flags (can be set from the editor).
		ENTITY_FLAG_CASTSHADOW = BIT32(0), //!< Indicates that the entity can cast shadows.
		ENTITY_FLAG_UNREMOVABLE = BIT32(1), //!< This entity cannot be removed using IEntitySystem::RemoveEntity until this flag is cleared.
		ENTITY_FLAG_GOOD_OCCLUDER = BIT32(2),       //!< Indicates that the entity can be very effective as an occluder.
		ENTITY_FLAG_NO_DECALNODE_DECALS = BIT32(3), //!< Indicates that the entity can't receive static decal projections.
		//////////////////////////////////////////////////////////////////////////

		ENTITY_FLAG_CLONED                  = BIT32(4), //!< Entity was cloned from another
		ENTITY_FLAG_NOT_REGISTER_IN_SECTORS = BIT32(5),
		ENTITY_FLAG_CALC_PHYSICS            = BIT32(6),
		//! Entity should only be present on the client, and not server.
		ENTITY_FLAG_CLIENT_ONLY = BIT32(7),
		//! Entity should only be present on the server, and not clients.
		ENTITY_FLAG_SERVER_ONLY = BIT32(8),
		ENTITY_FLAG_CUSTOM_VIEWDIST_RATIO = BIT32(9), //!< This entity has a special custom view distance ratio (AI/Vehicles require it).
		ENTITY_FLAG_CALCBBOX_USEALL = BIT32(10), //!< Use character and objects in BBox calculations.
		ENTITY_FLAG_VOLUME_SOUND  = BIT32(11), //!< Entity is a volume sound (will get moved around by the sound proxy).
		ENTITY_FLAG_HAS_AI        = BIT32(12), //!< Entity has an AI object.
		ENTITY_FLAG_TRIGGER_AREAS = BIT32(13), //!< This entity will trigger areas when it enters them.
		ENTITY_FLAG_NO_SAVE       = BIT32(14), //!< This entity will not be saved.
		ENTITY_FLAG_CAMERA_SOURCE = BIT32(15), //!< This entity is a camera source.
		ENTITY_FLAG_CLIENTSIDE_STATE = BIT32(16), //!< Prevents error when state changes on the client and does not sync state changes to the client.
		ENTITY_FLAG_SEND_RENDER_EVENT = BIT32(17), //!< When set entity will send ENTITY_EVENT_RENDER_VISIBILITY_CHANGE when starts or stop actual rendering.
		ENTITY_FLAG_NO_PROXIMITY = BIT32(18), //!< Entity will not be registered in the partition grid and can not be found by proximity queries.
		ENTITY_FLAG_PROCEDURAL = BIT32(19),    //!< Entity has been generated at runtime.
		ENTITY_FLAG_UPDATE_HIDDEN = BIT32(20), //!< Whether update of game logic should be skipped when the entity is hidden. This does *not* disable update of components unless they specifically request it.
		ENTITY_FLAG_IGNORE_PHYSICS_UPDATE = BIT32(21), //!< Used by the Editor only, don't set.
		ENTITY_FLAG_SPAWNED               = BIT32(22), //!< Entity was spawned dynamically without a class.
		ENTITY_FLAG_SLOTS_CHANGED         = BIT32(23), //!< Entity's slots were changed dynamically.
		ENTITY_FLAG_MODIFIED_BY_PHYSICS   = BIT32(24), //!< Entity was procedurally modified by physics.
		ENTITY_FLAG_OUTDOORONLY           = BIT32(25), //!< Same as Brush->Outdoor only.

		ENTITY_FLAG_RECVWIND     = BIT32(26), //!< Receives wind.
		ENTITY_FLAG_LOCAL_PLAYER = BIT32(27), //!< Indicates the entity is a local player.
		ENTITY_FLAG_AI_HIDEABLE  = BIT32(28), //!< AI can use the object to calculate automatic hide points.

		ENTITY_FLAG_DYNAMIC_DISTANCE_SHADOWS = BIT32(29),
	};

#pragma pack(push, 1)

	struct CEntity
	{
		virtual ~CEntity()                      = default; // offset 0
		virtual uint32_t GetId()                = 0;       // offset 8
		virtual __int64 GetGuid()               = 0;       // offset 16
		virtual EntityClass *GetClass()         = 0;       // offset 24
		virtual void GetArchetype()             = 0;
		virtual void SetFlags(uint32_t flags)   = 0;
		virtual uint32_t GetFlags()             = 0;
		virtual void AddFlags()                 = 0;
		virtual void ClearFlags()               = 0;
		virtual void CheckFlags()               = 0;
		virtual void SetFlagsExtended()         = 0;
		virtual void GetFlagsExtended()         = 0;
		virtual void IsGarbage()                = 0;
		virtual void Pad13()                    = 0;
		virtual void Pad14()                    = 0;
		virtual void Pad15()                    = 0;
		virtual void Pad16()                    = 0;
		virtual void SetName()                  = 0;
		virtual const char *GetName()           = 0; // offset 144
		virtual void GetEntityTextDescription() = 0;
		virtual void Pad20()                    = 0;
		virtual void Pad21()                    = 0;
		virtual void Pad22()                    = 0;
		virtual void Pad23()                    = 0;
		virtual void Pad24()                    = 0;
		virtual void Pad25()                    = 0;
		virtual void Pad26()                    = 0;
		virtual void Pad27()                    = 0;
		virtual void Pad28()                    = 0;
		virtual void Pad29()                    = 0;
		virtual void Pad30()                    = 0;
		virtual void Pad31()                    = 0;
		virtual void Pad32()                    = 0;
		virtual void Pad33()                    = 0;
		virtual void Pad34()                    = 0;
		virtual void Pad35()                    = 0;
		virtual void Pad36()                    = 0;
		virtual void Pad37()                    = 0;
		virtual void Pad38()                    = 0;
		virtual void Pad39()                    = 0;
		virtual void Pad40()                    = 0;
		virtual void Pad41()                    = 0;
		virtual void Pad42()                    = 0;
		virtual void Pad43()                    = 0;
		virtual void Pad44()                    = 0;
		virtual void SetPos()                   = 0;
		virtual void GetPos(Vec3 &pos)          = 0; // offset 368
		virtual void Pad47()                    = 0;
		virtual void Pad48()                    = 0;
		virtual void Pad49()                    = 0;
		virtual void Pad50()                    = 0;
		virtual void Pad51()                    = 0;
		virtual void Activate(bool active)      = 0;
		virtual bool IsActive()                 = 0; // offset 424
		virtual void Pad54()                    = 0;
		virtual void Pad55()                    = 0;
		virtual void Pad56()                    = 0;
		virtual void Serialize()                = 0;
		virtual void Pad58()                    = 0;
		virtual void SetGeometry()              = 0;
		virtual void Pad60()                    = 0;
		virtual void Pad61()                    = 0;
		virtual void Pad62()                    = 0;
		virtual void Hide(bool hide)            = 0;
		virtual bool IsHidden()                 = 0; // offset 512 - Pad64
		virtual void Invisible()                = 0;
		virtual void IsInvisible()              = 0;
		virtual void GetAI()                    = 0;
		virtual void HasAI()                    = 0;
		virtual void GetAIObjectID()            = 0;
		virtual void SetAIObjectID()            = 0;
		virtual void RegisterInAISystem()       = 0;
		virtual void SetUpdatePolicy()          = 0;
		virtual void GetUpdatePolicy()          = 0;
		virtual void GetProxy()                 = 0;
		virtual void SetProxy()                 = 0;
		virtual void CreateProxy()              = 0;
		virtual void RegisterComponent()        = 0;
		virtual void Physicalize()              = 0;
		// The offset of this one can be found through the usage of CScriptBind_Physics::RayWorldIntersection(IFunctionHandler* pH)
		virtual void *GetPhysics() = 0; // offset 632 - Function Number 79

		uint64_t unk_1;
		uint64_t unk_2;
		uint64_t unk_3;
		uint64_t unk_4;
		Archetype *m_archetype;
		uint64_t unk_5;
		uint64_t unk_6;
		uint64_t unk_7;
		uint64_t unk_8;
		uint64_t unk_9;
		uint64_t unk_10;
		uint32_t unk_11;
		float m_world_pos_x;
		uint64_t unk_12;
		uint32_t unk_13;
		float m_world_pos_y;
		uint64_t unk_14;
		uint32_t unk_15;
		float m_world_pos_z;

		inline Vec3 GetWorldPos()
		{
			Vec3 res;

			res.x = m_world_pos_x;
			res.y = m_world_pos_y;
			res.z = m_world_pos_z;

			//LOG(INFO) << res.x << " " << res.y << " " << res.z;
			//Logger::FlushQueue();

			return res;
		}

		inline void SetWorldPos(const Vec3 &newPos)
		{
			float matrix[3 * 4];
			memcpy(matrix, reinterpret_cast<const std::byte *>(this) + 88, sizeof(matrix));

			matrix[3]  = newPos.x;
			matrix[7]  = newPos.y;
			matrix[11] = newPos.z;

			g_CEntity_SetWorldTM(this, matrix, 0x40'00'00);
		}

		inline void GetWorldTM(float (&matrix)[3 * 4]) const
		{
			memcpy(matrix, reinterpret_cast<const std::byte *>(this) + 88, sizeof(matrix));
		}

		inline void SetWorldTM(const float (&matrix)[3 * 4])
		{
			float copy[3 * 4];
			memcpy(copy, matrix, sizeof(copy));
			g_CEntity_SetWorldTM(this, copy, 0x40'00'00);
		}
	};

#pragma pack(pop)

	static_assert(offsetof(CEntity, m_world_pos_x) == 100);
	static_assert(offsetof(CEntity, m_world_pos_y) == 116);
	static_assert(offsetof(CEntity, m_world_pos_z) == 132);
	static_assert(offsetof(CEntity, m_archetype) == 40);

	inline std::vector<CEntity *> g_entities;

	inline ankerl::unordered_dense::set<uintptr_t> g_cstatobjs;

	inline ankerl::unordered_dense::set<IRenderNode *> g_CGeomCacheRenderNodes;

	inline ankerl::unordered_dense::set<IRenderNode *> g_CVegetations;

	inline std::vector<IRenderNode *> g_CMergedMeshRenderNodes;

	inline toml_v2::config_file::config_entry<bool> *g_show_entity_inspector                = nullptr;
	inline toml_v2::config_file::config_entry<bool> *g_show_entity_metadata_inspector       = nullptr;
	inline toml_v2::config_file::config_entry<bool> *g_show_entity_xml_infos_inspector      = nullptr;
	inline toml_v2::config_file::config_entry<bool> *g_show_cbrush_inspector                = nullptr;
	inline toml_v2::config_file::config_entry<bool> *g_show_CMergedMeshRenderNode_inspector = nullptr;
	inline toml_v2::config_file::config_entry<bool> *g_show_ptf_inspector                   = nullptr;

	inline hotkey g_target_entity_on_crosshair("target_entity_on_crosshair", 0);

	inline toml_v2::config_file::config_entry<bool> *g_noclip_enabled = nullptr;
	inline hotkey g_noclip_enabled_keybind("noclip_enabled", 0);
	inline hotkey g_noclip_forward("noclip_forward", 'W');
	inline hotkey g_noclip_backward("noclip_backward", 'S');
	inline hotkey g_noclip_left("noclip_left", 'A');
	inline hotkey g_noclip_right("noclip_right", 'D');
	inline hotkey g_noclip_up("noclip_up", VK_SPACE);
	inline hotkey g_noclip_down("noclip_down", VK_CONTROL);
	inline toml_v2::config_file::config_entry<double> *g_noclip_speed_default    = nullptr;
	inline toml_v2::config_file::config_entry<double> *g_noclip_speed_multiplier = nullptr;
	inline hotkey g_noclip_speed_multiplier_keybind("noclip_speed_multiplier_keybind", VK_SHIFT);

	struct CEntitySystem
	{
		virtual ~CEntitySystem()                            = default;
		virtual void Pad1()                                 = 0;
		virtual void Pad2()                                 = 0;
		virtual void Pad3()                                 = 0;
		virtual void Pad4()                                 = 0;
		virtual void Pad5()                                 = 0;
		virtual void Pad6()                                 = 0;
		virtual void Pad7()                                 = 0;
		virtual void Pad8()                                 = 0;
		virtual void Pad9()                                 = 0;
		virtual void Pad10()                                = 0;
		virtual void Pad11()                                = 0;
		// The parameter block remains opaque until the KCD2-specific
		// SEntitySpawnParams layout has been audited.
		virtual CEntity *SpawnEntity(void *params, bool auto_init) = 0;
		virtual void Pad13()                                = 0;
		virtual void Pad14()                                = 0;
		virtual void Pad15()                                = 0;
		virtual void Pad16()                                = 0;
		virtual void Pad17()                                = 0;
		virtual void Pad18()                                = 0;
		virtual void RemoveEntity(uint32_t entity_id, bool force_remove_now) = 0;
		virtual void Pad20()                                = 0;
		virtual __int64 GetEntityIterator()                 = 0;
		virtual void Pad22()                                = 0;
		virtual void Pad23()                                = 0;
		virtual void Pad24()                                = 0;
		virtual void Pad25()                                = 0;
		virtual void Pad26()                                = 0;
		virtual void Pad27()                                = 0;
		virtual void Pad28()                                = 0;
		virtual void Pad29()                                = 0;
		virtual void Pad30()                                = 0;
		virtual void Pad31()                                = 0;
		virtual void Pad32()                                = 0;
		virtual void Pad33()                                = 0;
		virtual void Pad34()                                = 0;
		virtual void Pad35()                                = 0;
		virtual void Pad36()                                = 0;
		virtual void Pad37()                                = 0;
		virtual void Pad38()                                = 0;
		virtual void Pad39()                                = 0;
		virtual void Pad40()                                = 0;
		virtual void Pad41()                                = 0;
		virtual void Pad42()                                = 0;
		virtual void Pad43()                                = 0;
		virtual void Pad44()                                = 0;
		virtual void Pad45()                                = 0;
		virtual void Pad46()                                = 0;
		virtual void Pad47()                                = 0;
		virtual void Pad48()                                = 0;
		virtual void Pad49()                                = 0;
		virtual void Pad50()                                = 0;
		virtual void Pad51()                                = 0;
		virtual void Pad52()                                = 0;
		virtual void Pad53()                                = 0;
		virtual void Pad54()                                = 0;
		virtual void Pad55()                                = 0;
		virtual void Pad56()                                = 0;
		virtual void Pad57()                                = 0;
		virtual void Pad58()                                = 0;
		virtual void Pad59()                                = 0;
		virtual void Pad60()                                = 0;
		virtual void Pad61()                                = 0;
		virtual void Pad62()                                = 0;
		virtual void Pad63()                                = 0;
		virtual void Pad64()                                = 0;
		virtual void Pad65()                                = 0;
		virtual void Pad66()                                = 0;
		virtual void Pad67()                                = 0;
		virtual void Pad68()                                = 0;
		virtual void Pad69()                                = 0;
		virtual void Pad70()                                = 0;
		virtual __int64 GetEntityLayerData(CEntity *entity) = 0;
	};

	inline CEntitySystem *g_CEntitySystem = nullptr;

	struct xml_node_metadata_t
	{
		// id is parent id + this name
		std::string m_id;

		std::string m_name;

		// attributes and their potential values
		std::map<std::string, std::set<std::string>> m_attributes;
		// attributes and the EntityClass they are available on.
		std::map<std::string, std::set<std::string>> m_attributes_entityclass;

		// potential children ids.
		std::set<std::string> m_childrens;
		// children ids and the EntityClass they are available on.
		std::map<std::string, std::set<std::string>> m_childrens_entityclass;
	};

	struct entity_xml_info_t
	{
		// id is parent id + this name
		// is same as xml_node_metadata_t, for retrieving potential attributes, childrens, values, and so on for the Editor / Add / Modify Data part.
		std::string m_id;

		std::string m_name;

		// Only used by Root Entity xml nodes.
		std::string m_entity_name;
		// Only used by Root Entity xml nodes.
		std::string m_entity_class_name;

		// attribute name + attribute value
		std::vector<std::pair<std::string, std::string>> m_attributes;

		// childrens
		std::vector<entity_xml_info_t *> m_childrens;
	};

	struct EntityInfo
	{
		std::string name;
		std::string name_lower;
		std::string class_name;
		std::string class_name_lower;
		std::string archetype_name;
		std::string archetype_name_lower;
		uint32_t id;
		std::string id_string;
		uint32_t id_mask;
		uint32_t id_salt;
		std::string layer_name;
		std::string layer_name_lower;
		uint16_t layer_id = 0;
		std::string guid;
	};

	inline ankerl::unordered_dense::map<CEntity *, EntityInfo> g_entity_infos;
	inline std::vector<CBrush *> g_cbrushes;

	inline std::recursive_mutex g_xml_info_mutex;
	inline std::map<decltype(xml_node_metadata_t::m_id), xml_node_metadata_t> g_entity_xml_metadata;
	inline ankerl::unordered_dense::map<std::string, entity_xml_info_t *> g_entity_guid_to_xml_infos;
	inline std::vector<entity_xml_info_t *> g_entity_xml_infos;

	inline std::vector<int> g_entities_filtered;

	inline uintptr_t g_CD3D9Renderer = 0;

	inline uintptr_t g_CTerrain = 0;

	using CD3D9Renderer_ProjectToScreen_t = bool (*)(uintptr_t this_, float ptx, float pty, float ptz, float *sx, float *sy, float *sz);
	/*
		ptx (float) - world x-coordinate.
		pty (float) - world y-coordinate.
		ptz (float) - world z-coordinate.
		sx (float)* - Pointer to the resulting screen space x-coordinate.
		sy (float)* - Pointer to the resulting screen space x-coordinate.
		sz (float)* - Pointer to the depth value (typically between 0.0 and 1.0 in normalized device coordinates).
		Returns true if successful.
	*/
	inline CD3D9Renderer_ProjectToScreen_t g_CD3D9Renderer_ProjectToScreen = nullptr;

	using CD3D9Renderer_UnProjectFromScreen_t = int (*)(void *this_, float sx, float sy, float sz, float *px, float *py, float *pz);
	/*
		sx (float) - The x-coordinate in screen space.
		sy (float) - The y-coordinate in screen space.
		sz (float) - The depth value (typically between 0.0 and 1.0 in normalized device coordinates).
		px (float)* - Pointer to the resulting world x-coordinate.
		py (float)* - Pointer to the resulting world y-coordinate.
		pz (float)* - Pointer to the resulting world z-coordinate.
	*/
	inline CD3D9Renderer_UnProjectFromScreen_t g_CD3D9Renderer_UnProjectFromScreen = nullptr;

	enum pe_type
	{
		PE_NONE           = 0,
		PE_STATIC         = 1,
		PE_RIGID          = 2,
		PE_WHEELEDVEHICLE = 3,
		PE_LIVING         = 4,
		PE_PARTICLE       = 5,
		PE_ARTICULATED    = 6,
		PE_ROPE           = 7,
		PE_SOFT           = 8,
		PE_AREA           = 9,
		PE_GRID           = 10,
		PE_WALKINGRIGID   = 11
	};

	struct pe_params
	{
		int type;
	};

	enum EPE_Params
	{
		ePE_params_pos                         = 0,
		ePE_player_dimensions                  = 1,
		ePE_params_car                         = 2,
		ePE_params_particle                    = 3,
		ePE_player_dynamics                    = 4,
		ePE_params_joint                       = 5,
		ePE_params_part                        = 6,
		ePE_params_sensors                     = 7,
		ePE_params_articulated_body            = 8,
		ePE_params_outer_entity                = 9,
		ePE_simulation_params                  = 10,
		ePE_params_foreign_data                = 11,
		ePE_params_buoyancy                    = 12,
		ePE_params_rope                        = 13,
		ePE_params_bbox                        = 14,
		ePE_params_flags                       = 15,
		ePE_params_wheel                       = 16,
		ePE_params_softbody                    = 17,
		ePE_params_area                        = 18,
		ePE_tetrlattice_params                 = 19,
		ePE_params_ground_plane                = 20,
		ePE_params_structural_joint            = 21,
		ePE_params_waterman                    = 22,
		ePE_params_timeout                     = 23,
		ePE_params_skeleton                    = 24,
		ePE_params_structural_initial_velocity = 25,
		ePE_params_collision_class             = 26,
		ePE_params_walking_rigid               = 27,

		ePE_Params_Count
	};

	struct pe_params_part : pe_params
	{
		enum entype
		{
			type_id = ePE_params_part
		};

		pe_params_part()
		{
			type         = type_id;
			pMtx3x4      = 0;
			pMtx3x3      = 0;
			bRecalcBBox  = 1;
			bAddrefGeoms = 0;
			flagsOR = flagsColliderOR = 0;
			flagsAND = flagsColliderAND = (unsigned)-1;
		}

		int partid;      //!< Identifier of part.
		int ipart;       //!< Optionally, internal part slot number.
		int bRecalcBBox; //!< Whether entity's bounding box should be recalculated.
		Vec3 pos;
		float q[4];
		float scale;
		void *pMtx3x4;                  //!< Optional position+orientation.
		void *pMtx3x3;                  //!< Optional orientation via 3x3 matrix.
		unsigned int flagsCond;         //!< If partid and ipart are not specified, check for parts with flagsCond set.
		unsigned int flagsOR, flagsAND; //!< New flags = (flags & flagsAND) | flagsOR.
		unsigned int flagsColliderOR, flagsColliderAND;
		float mass; //!< Either mass of density should be set; mass = density*volume.
		float density;
		float minContactDist;                             //!< Threshold for contact points generation.
		struct phys_geometry *pPhysGeom, *pPhysGeomProxy; //!< If present and different from pPhysGeomProxy, pPhysGeom is used for raytracing.
		int idmatBreakable; //!< If >=0, the part is procedurally breakable with this mat_id (see AddExplosionShape).
		void *pLattice;     //!< Lattice is used for soft bodies and procedural structural breaking.
		int idSkeleton;     //!< Part with this id becomes this part's deformation skeleton.
		int *pMatMapping;   //!< Material mapping table for this part.
		int nMats;          //!< Number of pMatMapping entries.
		float invTimeStep;  //!< 1.0f/time_step, ragdolls will compute joint's velocity if this and position is set.
		int bAddrefGeoms;   //!< AddRef returned geometries if used in GetParams.
		int idParent; //!< Parent for hierarchical breaking; it hides all children until at least one of them breaks off.
	};

	struct pe_params_pos : pe_params
	{
		enum entype
		{
			type_id = ePE_params_pos
		};

		pe_params_pos()
		{
			type           = type_id;
			pMtx3x4        = 0;
			pMtx3x3        = 0;
			bRecalcBounds  = 1;
			bEntGridUseOBB = 0;
		}

		Vec3 pos;
		float q[4];
		float scale;   //!< Note that since there's no per-entity scale, it gets 'baked' into individual parts' scales.
		void *pMtx3x4; //!< Optional position+orientation.
		void *pMtx3x3; //!< Optional orientation via 3x3 matrix.
		int iSimClass; //!< See the sim_class enum.
		int bRecalcBounds; //!< Tells to recompute the bounding boxes.
		bool bEntGridUseOBB; //!< Whether or not to use part OBBs rather than object AABB when registering in the entity grid.
		bool doubleBufCoords; //!< Maintain pos and q from the last poststep sync, which can be used in world queries and pe_status_pos

		const IPhysicalEntity *pGridRefEnt; //!< New grid (set via a reference entity)
	};

	//! Sets or gets the bounding box of a physical entity
	struct pe_params_bbox : pe_params
	{
		enum entype
		{
			type_id = ePE_params_bbox
		};

		pe_params_bbox()
		{
			type = type_id;
		}

		AABB BBox; //!< Force this bounding box (note that if the entity recomputes it later, it will override this).
	};

	struct pe_params_outer_entity : pe_params
	{
		enum entype
		{
			type_id = ePE_params_outer_entity
		};

		IPhysicalEntity *pOuterEntity; //!< Outer entity is used to group together SC_INDEPENDENT entities (example: ropes on a tree trunk) and order their updates.
	};

	struct pe_status
	{
		int type;
	};

	//! IDs that can be used for foreign id.
	enum EPhysicsForeignIds
	{
		PHYS_FOREIGN_ID_TERRAIN                  = 0,
		PHYS_FOREIGN_ID_STATIC                   = 1,
		PHYS_FOREIGN_ID_ENTITY                   = 2,
		PHYS_FOREIGN_ID_FOLIAGE                  = 3,
		PHYS_FOREIGN_ID_ROPE                     = 4,
		PHYS_FOREIGN_ID_SOUND_OBSTRUCTION        = 5,
		PHYS_FOREIGN_ID_SOUND_PROXY_OBSTRUCTION  = 6,
		PHYS_FOREIGN_ID_SOUND_REVERB_OBSTRUCTION = 7,
		PHYS_FOREIGN_ID_WATERVOLUME              = 8,
		PHYS_FOREIGN_ID_BREAKABLE_GLASS          = 9,
		PHYS_FOREIGN_ID_BREAKABLE_GLASS_FRAGMENT = 10,
		PHYS_FOREIGN_ID_RIGID_PARTICLE           = 11,
		PHYS_FOREIGN_ID_RESERVED1                = 12,
		PHYS_FOREIGN_ID_RAGDOLL                  = 13,

		PHYS_FOREIGN_ID_USER = 100, //!< All user defined foreign ids should start from this enum.
	};

	struct IPhysicalEntity
	{
		virtual ~IPhysicalEntity()                                                            = default;
		virtual pe_type GetType()                                                             = 0;
		virtual int AddRef()                                                                  = 0;
		virtual int Release()                                                                 = 0;
		virtual int SetParams(pe_params *params, int bThreadSafe = 0)                         = 0;
		virtual int GetParams(pe_params *params)                                              = 0;
		virtual void GetStatus(pe_status *status)                                             = 0;
		virtual void Action(void *, int bThreadSafe = 0)                                      = 0;
		virtual void AddGeometry(void *pgeom, void *params, int id = -1, int bThreadSafe = 0) = 0;
		virtual void RemoveGeometry(int id, int bThreadSafe = 0)                              = 0;

		// returns entity's pForeignData if itype matches iForeignData, 0 otherwise
		virtual void *GetForeignData(int itype) = 0;
		virtual int GetiForeignData()           = 0;

		virtual void GetStateSnapshot()    = 0;
		virtual void Pad13()               = 0;
		virtual void Pad14()               = 0;
		virtual void Pad15()               = 0;
		virtual void Pad16()               = 0;
		virtual void Pad17()               = 0;
		virtual void Pad18()               = 0;
		virtual void Pad19()               = 0;
		virtual void Pad20()               = 0;
		virtual void Pad21()               = 0;
		virtual void Pad22()               = 0;
		virtual void Pad23()               = 0;
		virtual void Pad24()               = 0;
		virtual void Pad25()               = 0;
		virtual void Pad26()               = 0;
		virtual void Pad27()               = 0;
		virtual void Pad28()               = 0;
		virtual void Pad29()               = 0;
		virtual void Pad30()               = 0;
		virtual void Pad31()               = 0;
		virtual void Pad32()               = 0;
		virtual void Pad33()               = 0;
		virtual void Pad34()               = 0;
		virtual void Pad35()               = 0;
		virtual void Pad36()               = 0;
		virtual void Pad37()               = 0;
		virtual void Pad38()               = 0;
		virtual void Pad39()               = 0;
		virtual void Pad40()               = 0;
		virtual void Pad41()               = 0;
		virtual void Pad42()               = 0;
		virtual void Pad43()               = 0;
		virtual void Pad44()               = 0;
		virtual void Pad45()               = 0;
		virtual void Pad46()               = 0;
		virtual void Pad47()               = 0;
		virtual void Pad48()               = 0;
		virtual void Pad49()               = 0;
		virtual void Pad50()               = 0;
		virtual void Pad51()               = 0;
		virtual void Pad52()               = 0;
		virtual void Pad53()               = 0;
		virtual void Pad54()               = 0;
		virtual void Pad55()               = 0;
		virtual void Pad56()               = 0;
		virtual void Pad57()               = 0;
		virtual void Pad58()               = 0;
		virtual void Pad59()               = 0;
		virtual void Pad60()               = 0;
		virtual void Pad61()               = 0;
		virtual void Pad62()               = 0;
		virtual void Pad63()               = 0;
		virtual void Pad64()               = 0;
		virtual void Pad65()               = 0;
		virtual void Pad66()               = 0;
		virtual void Pad67()               = 0;
		virtual int IsPointInside(Vec3 pt) = 0;
	};

	struct ray_hit_t
	{
		float dist;

		IPhysicalEntity *pCollider;

		int ipart;
		int partid;
		short surface_idx;
		short idmatOrg;
		int foreignIdx;
		int iNode;
		Vec3 pt;
		Vec3 n;
		int bTerrain;
		int iPrim;
		ray_hit_t *next;
	};

	enum geom_flags
	{
		//! collisions between parts are checked if (part0->flagsCollider & part1->flags) !=0
		geom_colltype0    = 0x00'01,
		geom_colltype1    = 0x00'02,
		geom_colltype2    = 0x00'04,
		geom_colltype3    = 0x00'08,
		geom_colltype4    = 0x00'10,
		geom_colltype5    = 0x00'20,
		geom_colltype6    = 0x00'40,
		geom_colltype7    = 0x00'80,
		geom_colltype8    = 0x01'00,
		geom_colltype9    = 0x02'00,
		geom_colltype10   = 0x04'00,
		geom_colltype11   = 0x08'00,
		geom_colltype12   = 0x10'00,
		geom_colltype13   = 0x20'00,
		geom_colltype14   = 0x40'00,
		geom_colltype_ray = 0x80'00,   //!< special colltype used by raytracing by default
		geom_floats       = 0x1'00'00, //!< colltype required to apply buoyancy
		geom_proxy = 0x2'00'00, //!< only used in AddGeometry to specify that this geometry should go to pPhysGeomProxy
		geom_structure_changes = 0x4'00'00,  //!< part is breaking/deforming
		geom_can_modify        = 0x8'00'00,  //!< geometry is cloned and is used in this entity only
		geom_squashy           = 0x10'00'00, //!< part has 'soft' collisions (used for tree foliage proxy)
		geom_log_interactions = 0x20'00'00, //!< part will post EventPhysBBoxOverlap whenever something happens inside its bbox
		geom_monitor_contacts   = 0x40'00'00,   //!< part needs collision callback from the solver (used internally)
		geom_manually_breakable = 0x80'00'00,   //!< part is breakable outside the physics
		geom_no_coll_response   = 0x1'00'00'00, //!< collisions are detected and reported, but not processed
		geom_mat_substitutor = 0x2'00'00'00, //!< geometry is used to change other collision's material id if the collision point is inside it
		geom_break_approximation = 0x4'00'00'00, //!< applies capsule approximation after breaking (used for tree trunks)
		geom_no_particle_impulse = 0x8'00'00'00, //!< phys particles don't apply impulses to this part; should be used in flagsCollider
		geom_destroyed_on_break = 0x2'00'00'00,  //!< should be used in flagsCollider
		geom_allow_id_duplicates = 0x1'00'00'00, //!< doesn't check if id is already used; should be set in flagsCollider
		geom_ignore_BBox = 0x20'00'00'00,        //!< don't include the part in entity world BBox computation
		//! mnemonic group names
		geom_colltype_player        = geom_colltype1,
		geom_colltype_explosion     = geom_colltype2,
		geom_colltype_vehicle       = geom_colltype3,
		geom_colltype_foliage       = geom_colltype4,
		geom_colltype_debris        = geom_colltype5,
		geom_colltype_foliage_proxy = geom_colltype13,
		geom_colltype_obstruct      = geom_colltype14,
		geom_colltype_solid         = 0x0F'FF & ~geom_colltype_explosion,
		geom_collides               = 0xFF'FF
	};

	enum surface_flags
	{
		sf_pierceable_mask    = 0x0F,
		sf_max_pierceable     = 0x0F,
		sf_important          = 0x2'00,
		sf_manually_breakable = 0x4'00,
		sf_matbreakable_bit   = 16
	};

	enum rwi_flags
	{
		rwi_ignore_terrain_holes    = 0x20,
		rwi_ignore_noncolliding     = 0x40,
		rwi_ignore_back_faces       = 0x80,
		rwi_ignore_solid_back_faces = 0x1'00,
		rwi_pierceability_mask      = 0x0F,
		rwi_pierceability0          = 0,
		rwi_stop_at_pierceable      = 0x0F,
		rwi_max_piercing            = 0x10, //!< the ray will pierce all surfaces (including those with pierceability 0)
		rwi_separate_important_hits = sf_important, //!< among pierceble hits, materials with sf_important will have priority
		rwi_colltype_bit = 16, //!< used to manually specify collision geometry types (default is geom_colltype_ray)
		rwi_colltype_any = 0x4'00, //!< if several colltype flag are specified, switches between requiring all or any of them in a geometry
		rwi_queue = 0x8'00, //!< queues the RWI request, when done it'll generate EventPhysRWIResult
		rwi_force_pierceable_noncoll = 0x10'00, //!< non-colliding geometries will be treated as pierceable regardless of the actual material
		rwi_update_last_hit = 0x40'00, //!< update phitLast with the current hit results (should be set if the last hit should be reused for a "warm" start)
		rwi_any_hit = 0x80'00 //!< returns the first found hit for meshes, not necessarily the closets
	};

	enum phentity_flags
	{
		// PE_PARTICLE-specific flags
		particle_single_contact       = 0x01, //!< Full stop after first contact.
		particle_constant_orientation = 0x02, //!< Forces constant orientation.
		particle_no_roll = 0x04, //!< 'sliding' mode; entity's 'normal' vector axis will be alinged with the ground normal.
		particle_no_path_alignment = 0x08, //!< Unless set, entity's y axis will be aligned along the movement trajectory.
		particle_no_spin            = 0x10,   //!< Disables spinning while flying.
		particle_no_self_collisions = 0x1'00, //!< Disables collisions with other particles.
		particle_no_impulse = 0x2'00, //!< Particle will not add hit impulse (expecting that some other system will).

		// PE_LIVING-specific flags
		lef_push_objects = 0x01,
		lef_push_players = 0x02, //!< Push objects and players during contacts.
		lef_snap_velocities = 0x04, //!< Quantizes velocities after each step (was ised in MP for precise deterministic sync).
		lef_loosen_stuck_checks = 0x08, //!< Don't do additional intersection checks after each step (recommended for NPCs to improve performance).
		lef_report_sliding_contacts = 0x10, //!< Unless set, 'grazing' contacts are not reported.

		// PE_ROPE-specific flags
		rope_findiff_attached_vel = 0x01, //!< Approximate velocity of the parent object as v = (pos1-pos0)/time_interval.
		rope_no_solver = 0x02, //!< No velocity solver; will rely on stiffness (if set) and positional length enforcement.
		rope_ignore_attachments = 0x4, //!< No collisions with objects the rope is attached to.
		rope_target_vtx_rel0    = 0x08,
		rope_target_vtx_rel1    = 0x10, //!< Whether target vertices are set in the parent entity's frame.
		rope_subdivide_segs = 0x1'00, //!< Turns on 'dynamic subdivision' mode (only in this mode contacts in a strained state are handled correctly).
		rope_no_tears              = 0x2'00,     //!< Rope will not tear when it reaches its force limit, but stretch.
		rope_collides              = 0x20'00'00, //!< Rope will collide with objects other than the terrain.
		rope_collides_with_terrain = 0x40'00'00, //!< Rope will collide with the terrain.
		rope_collides_with_attachment = 0x80, //!< Rope will collide with the objects it's attached to even if the other collision flags are not set.
		rope_no_stiffness_when_colliding = 0x10'00'00'00, //!< Rope will use stiffness 0 if it has contacts.

		//! PE_SOFT-specific flags
		se_skip_longest_edges = 0x01, //!< the longest edge in each triangle with not participate in the solver
		se_rigid_core         = 0x02, //!< soft body will have an additional rigid body core

		//! PE_RIGID-specific flags (note that PE_ARTICULATED and PE_WHEELEDVEHICLE are derived from it)
		ref_use_simple_solver  = 0x01, //!< use penalty-based solver (obsolete)
		ref_no_splashes        = 0x04, //!< will not generate EventPhysCollisions when contacting water
		ref_checksum_received  = 0x04,
		ref_checksum_outofsync = 0x08, //!< obsolete
		ref_small_and_fast = 0x1'00, //!< entity will trace rays against alive characters; set internally unless overriden

		//! PE_ARTICULATED-specific flags
		aef_recorded_physics = 0x02, //!< specifies a an entity that contains pre-baked physics simulation

		//! PE_WHEELEDVEHICLE-specific flags
		wwef_fake_inner_wheels = 0x08, //!< exclude wheels between the first and the last one from the solver
		                               //! (only wheels with non-0 suspension are considered)

		//! general flags
		pef_parts_traceable     = 0x10,   //!< each entity part will be registered separately in the entity grid
		pef_disabled            = 0x20,   //!< entity will not be simulated
		pef_never_break         = 0x40,   //!< entity will not break or deform other objects
		pef_deforming           = 0x80,   //!< entity undergoes a dynamic breaking/deforming
		pef_pushable_by_players = 0x2'00, //!< entity can be pushed by playerd
		pef_traceable           = 0x4'00,
		particle_traceable      = 0x4'00,
		rope_traceable          = 0x4'00, //!< entity is registered in the entity grid
		pef_update = 0x8'00, //!< only entities with this flag are updated if ent_flagged_only is used in TimeStep()
		pef_monitor_state_changes = 0x10'00, //!< generate immediate events for simulation class changed (typically rigid bodies falling asleep)
		pef_monitor_collisions    = 0x20'00,   //!< generate immediate events for collisions
		pef_monitor_env_changes   = 0x40'00,   //!< generate immediate events when something breaks nearby
		pef_never_affect_triggers = 0x80'00,   //!< don't generate events when moving through triggers
		pef_invisible             = 0x1'00'00, //!< will apply certain optimizations for invisible entities
		pef_ignore_ocean          = 0x2'00'00, //!< entity will ignore global water area
		pef_fixed_damping         = 0x4'00'00, //!< entity will force its damping onto the entire group
		pef_monitor_poststep      = 0x8'00'00, //!< entity will generate immediate post step events
		pef_always_notify_on_deletion = 0x10'00'00, //!< when deleted, entity will awake objects around it even if it's not referenced (has refcount 0)
		pef_override_impulse_scale = 0x20'00'00, //!< entity will ignore breakImpulseScale in PhysVars
		pef_players_can_break      = 0x40'00'00, //!< playes can break the entiy by bumping into it
		pef_cannot_squash_players = 0x10'00'00'00, //!< entity will never trigger 'squashed' state when colliding with players
		pef_ignore_areas      = 0x80'00'00,        //!< entity will ignore phys areas (gravity and water)
		pef_log_state_changes = 0x1'00'00'00, //!< entity will log simulation class change events
		pef_log_collisions    = 0x2'00'00'00, //!< entity will log collision events
		pef_log_env_changes   = 0x4'00'00'00, //!< entity will log EventPhysEnvChange when something breaks nearby
		pef_log_poststep      = 0x8'00'00'00, //!< entity will log EventPhysPostStep events
	};

	enum entity_query_flags
	{
		ent_static         = 1,
		ent_sleeping_rigid = 2,
		ent_rigid          = 4,
		ent_living         = 8,
		ent_independent    = 16,
		ent_deleted        = 128,
		ent_terrain        = 0x1'00,
		ent_all            = ent_static | ent_sleeping_rigid | ent_rigid | ent_living | ent_independent | ent_terrain,
		ent_flagged_only   = pef_update,
		ent_skip_flagged   = pef_update * 2, //!< "flagged" meas has pef_update set
		ent_areas          = 32,
		ent_triggers       = 64,
		ent_ignore_noncolliding    = 0x1'00'00,
		ent_sort_by_mass           = 0x2'00'00, //!< sort by mass in ascending order
		ent_allocate_list          = 0x4'00'00, //!< if not set, the function will return an internal pointer
		ent_water                  = 0x2'00,    //!< can only be used in RayWorldIntersection
		ent_no_ondemand_activation = 0x8'00'00, //!< can only be used in RayWorldIntersection
		ent_delayed_deformations = 0x8'00'00, //!< queues procedural breakage requests; can only be used in SimulateExplosion
		ent_addref_results = 0x10'00'00, //!< will call AddRef on each entity in the list (expecting the caller call Release)
		ent_use_sync_coords = 0x20'00'00, //<! use entity coords that are sync'ed to the last poststep event pump
		ent_reserved2       = 0x20'00'00'00,
		ent_reserved1       = 0x40'00'00'00
	};

	void target_entity_on_crosshair();
	void target_entity_on_crosshair_include_cbrush();
	void target_entity_on_screen_cursor();
	inline int g_selected_index_entity_detail_inspector          = -1;
	inline int g_selected_cbrush_detail_inspector                = -1;
	inline int g_selected_CMergedMeshRenderNode_detail_inspector = -1;
	inline int g_imguizmo_editor_local_or_world                  = ImGuizmo::WORLD;

	void offset_every_object_around();

	inline uintptr_t g_C3DEngine = 0;

	inline ankerl::unordered_dense::set<IRenderNode *> g_rendernodes;

	inline ankerl::unordered_dense::set<IPhysicalEntity *> g_physicalentities;

	inline void C3DEngine_UnRegisterEntityDirect(IRenderNode *node)
	{
		// For finding the offset, look for string usage of "I3DEngine::DeleteVisArea: Invalid VisArea pointer"
		auto func = (*reinterpret_cast<void ***>(g_C3DEngine))[46];

		((void (*)(uintptr_t, IRenderNode *))func)(g_C3DEngine, node);
	}

	inline void C3DEngine_RegisterEntity(IRenderNode *node)
	{
		// For finding the offset, look for string usage of "I3DEngine::DeleteVisArea: Invalid VisArea pointer"
		auto func = (*reinterpret_cast<void ***>(g_C3DEngine))[38];

		((void (*)(uintptr_t, IRenderNode *))func)(g_C3DEngine, node);
	}

	inline uintptr_t g_ISystem         = 0;
	inline uintptr_t *g_gEnv_pGame_ptr = 0;

	inline uintptr_t g_C_PlayerStateMovement = 0;
	inline uintptr_t g_C_Player              = 0;

	using RayWorldIntersection_t = int (*)(__int64 IPhysicalWorld_instance, Vec3 *source, Vec3 *direction, unsigned int iEntTypes, unsigned int flags, ray_hit_t *hits, int nmaxhits, void **pSkipEnts, int nSkipEnts, __int64 pForeignData, int iForeignData, const char *pNameTag);
	inline RayWorldIntersection_t g_RayWorldIntersection = nullptr;

	inline int RayWorldIntersection(const Vec3 &source, const Vec3 &direction, unsigned int iEntTypes, unsigned int flags, ray_hit_t *hits, int nmaxhits, void **pSkipEnts = 0, int nSkipEnts = 0, __int64 pForeignData = 0, int iForeignData = 0, const char *pNameTag = "RayWorldIntersection(Physics)")
	{
		// ISystem_GetIPhysicalWorld_func vtable offset found in the function that call RayWorldIntersection and use "RayWorldIntersection(Script)" as parameter
		const auto ISystem_GetIPhysicalWorld_func = (*reinterpret_cast<void ***>(g_ISystem))[74];
		const auto IPhysicalWorld_instance        = ((__int64 (*)(uint64_t))ISystem_GetIPhysicalWorld_func)(g_ISystem);
		return g_RayWorldIntersection(IPhysicalWorld_instance, (Vec3 *)&source, (Vec3 *)&direction, iEntTypes, flags, hits, nmaxhits, pSkipEnts, nSkipEnts, pForeignData, iForeignData, pNameTag);
	}

	inline std::vector<std::string> g_lua_execute_buffer_queue;

	void CEntitySystem_DumpEntity(CEntitySystem *entity_system, CEntity *entity);
	void CEntitySystem_DumpEntities(CEntitySystem *CEntitySystem_ptr);

	inline ankerl::unordered_dense::set<std::string> g_fmod_events;

	using fmodstudio_loadbankfile_t = __int64 (*)(void *this_, const char *filename, __int64 fmod_studio_load_bank_flags, void **bank);
	inline fmodstudio_loadbankfile_t fmodstudio_loadbankfile_orig = nullptr;

	using fmodstudio_getevent_t = __int64 (*)(void *fmodstudio_event_system_this, const char *event_name, void **event_description_result);

	inline fmodstudio_getevent_t fmodstudio_getevent_orig = nullptr;

	using fmodstudio_bank_unload_t                              = __int64 (*)(void *this_);
	inline fmodstudio_bank_unload_t fmodstudio_bank_unload_orig = nullptr;

	inline void *g_fmodstudio_instance = nullptr;

	template<class T>
	inline void ForceWrite(T &dst, const T &src)
	{
		DWORD old_flag;
		::VirtualProtect(&dst, sizeof(T), PAGE_EXECUTE_READWRITE, &old_flag);
		dst = src;
		::VirtualProtect(&dst, sizeof(T), old_flag, &old_flag);
	}

	template<class F>
	inline bool EachImportFunction(HMODULE module, const char *dllname, const F &f)
	{
		if (module == 0)
		{
			return false;
		}

		size_t ImageBase             = (size_t)module;
		PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)ImageBase;
		if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE)
		{
			return false;
		}
		PIMAGE_NT_HEADERS pNTHeader = (PIMAGE_NT_HEADERS)(ImageBase + pDosHeader->e_lfanew);

		size_t RVAImports = pNTHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
		if (RVAImports == 0)
		{
			return false;
		}

		IMAGE_IMPORT_DESCRIPTOR *pImportDesc = (IMAGE_IMPORT_DESCRIPTOR *)(ImageBase + RVAImports);
		while (pImportDesc->Name != 0)
		{
			if (!dllname || stricmp((const char *)(ImageBase + pImportDesc->Name), dllname) == 0)
			{
				IMAGE_IMPORT_BY_NAME **func_names = (IMAGE_IMPORT_BY_NAME **)(ImageBase + pImportDesc->Characteristics);
				void **import_table               = (void **)(ImageBase + pImportDesc->FirstThunk);
				for (size_t i = 0;; ++i)
				{
					if ((size_t)func_names[i] == 0)
					{
						break;
					}
					const char *funcname = (const char *)(ImageBase + (size_t)func_names[i]->Name);
					f(funcname, import_table[i]);
				}
			}
			++pImportDesc;
		}
		return true;
	}

	struct kcd2_init_result
	{
		bool success{};
		size_t signatures_requested{};
		size_t signatures_resolved{};
		size_t derived_requested{};
		size_t derived_resolved{};
		std::vector<std::string> errors;
	};

	kcd2_init_result kcd2_init();
} // namespace big
