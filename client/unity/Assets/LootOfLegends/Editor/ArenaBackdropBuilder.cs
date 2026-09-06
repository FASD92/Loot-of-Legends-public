using System;
using System.Collections.Generic;
using System.Linq;
using UnityEditor;
using UnityEngine;
using UnityEngine.Tilemaps;

namespace LootOfLegends.Editor
{
    /// <summary>
    /// Rebuilds the Arena backdrop so the authoritative play field is legible on screen.
    ///
    /// The server clamps participant positions to plus or minus 10000 millimetres
    /// (BattleInstance.cpp), and the client renders one world unit per 1000 millimetres, so the
    /// reachable field is exactly the world square from -10 to 10. The Arena camera is orthographic
    /// with size 10.8, which at 16:9 shows 38.4 by 21.6 units: the field fills the height but only
    /// about half the width. Leaving the whole viewport as one flat grass sheet hides where a
    /// player may actually go, so this paints the field with packed dirt, rings it with a wall just
    /// outside the clamp, and fills the two out of bounds columns with village scenery.
    ///
    /// Tilemap contents are authored here rather than in prefab YAML because a 40 by 22 field is
    /// hundreds of cell entries with interlocking internal ids, and because sprite rects and Tile
    /// assets need engine generated identifiers to stay consistent.
    /// </summary>
    public static class ArenaBackdropBuilder
    {
        private const string MapRoot = "Assets/ThirdParty/PresentationPack/content/map/";
        private const string WallTexture = MapRoot + "tileset_wall_simple.png";
        private const string FloorTexture = MapRoot + "tileset_floor.png";
        private const string TileFolder = "Assets/Presentation/Tiles";
        private const string BackdropPrefab =
            "Assets/Presentation/Prefabs/World/ArenaBackdrop.prefab";
        private const string GrassTile = TileFolder + "/Ground.asset";
        private const string PropsGroup = "Props";

        // Authoritative play field in cells. Cell (i, j) spans world [i, i+1) so the world square
        // from -10 to 10 is cells -10 through 9 on both axes.
        private const int PlayMin = -10;
        private const int PlayMax = 9;

        // The wall sits one cell outside the clamp so it bounds the field without stealing a cell
        // a player can legally stand on.
        private const int RingMin = PlayMin - 1;
        private const int RingMax = PlayMax + 1;

        // ArenaBackdropCoversFixedCameraViewport requires the Ground cellBounds to reach
        // xMin <= -20, xMax >= 20, yMin <= -11, yMax >= 11 for a 10.8 half height at 16:9.
        private const int GroundMinX = -20;
        private const int GroundMaxX = 19;
        private const int GroundMinY = -11;
        private const int GroundMaxY = 10;

        private const int WindowEveryCells = 5;

        // tileset_wall_simple.png is 160x176 and both axes are exact multiples of 16, so a bottom
        // left origin rect lands on the art grid with no correction. These are the brown brick set.
        private static readonly (string Name, int X, int Y)[] WallSprites =
        {
            ("wall.brick.corner-top-left", 0, 64),
            ("wall.brick.top", 32, 64),
            ("wall.brick.corner-top-right", 64, 64),
            ("wall.brick.window", 16, 48),
            ("wall.brick.left", 0, 32),
            ("wall.brick.right", 64, 32),
            ("wall.brick.corner-bottom-left", 0, 0),
            ("wall.brick.bottom", 32, 0),
            ("wall.brick.corner-bottom-right", 64, 0)
        };

        // tileset_floor.png is 352x417 and the height is not a multiple of 16, so the art grid
        // starts one pixel up from a bottom aligned grid and every rect y must satisfy
        // y % 16 == 1. Both entries below are interior fill cells of the packed dirt block, using
        // only #90775e and #816855, which is why they read as ground rather than as a grass to
        // dirt transition the way ground.path does.
        private static readonly (string Name, int X, int Y)[] FloorSprites =
        {
            ("ground.dirt-a", 192, 161),
            ("ground.dirt-b", 224, 113)
        };

        private static readonly Dictionary<string, string> WallTileAssets = new()
        {
            ["wall.brick.corner-top-left"] = "ArenaWallCornerTopLeft",
            ["wall.brick.top"] = "ArenaWallTop",
            ["wall.brick.corner-top-right"] = "ArenaWallCornerTopRight",
            ["wall.brick.window"] = "ArenaWallWindow",
            ["wall.brick.left"] = "ArenaWallLeft",
            ["wall.brick.right"] = "ArenaWallRight",
            ["wall.brick.corner-bottom-left"] = "ArenaWallCornerBottomLeft",
            ["wall.brick.bottom"] = "ArenaWallBottom",
            ["wall.brick.corner-bottom-right"] = "ArenaWallCornerBottomRight"
        };

        private static readonly Dictionary<string, string> FloorTileAssets = new()
        {
            ["ground.dirt-a"] = "ArenaFloorA",
            ["ground.dirt-b"] = "ArenaFloorB"
        };

        public static void Build()
        {
            EnsureSprites(WallTexture, WallSprites);
            EnsureSprites(FloorTexture, FloorSprites);

            Dictionary<string, TileBase> tiles = CreateTiles(WallTexture, WallTileAssets);
            foreach ((string key, TileBase tile) in CreateTiles(FloorTexture, FloorTileAssets))
            {
                tiles.Add(key, tile);
            }

            PaintBackdrop(tiles);
            AssetDatabase.SaveAssets();
            Debug.Log("ArenaBackdropBuilder completed");
        }

        /// <summary>
        /// Adds or updates sprite rects by name while preserving every sprite already defined on
        /// the texture. Assigning TextureImporter.spritesheet replaces the whole list, so dropping
        /// an existing entry would break the Tile assets that reference it; the importer keeps
        /// internal ids stable across reimports as long as the names survive.
        /// </summary>
        private static void EnsureSprites(
            string texturePath,
            (string Name, int X, int Y)[] wanted)
        {
            TextureImporter importer = AssetImporter.GetAtPath(texturePath) as TextureImporter;
            if (importer == null)
            {
                throw new InvalidOperationException($"Missing texture importer for {texturePath}");
            }

            // Every map atlas gets the same settings so tiles from different atlases sample
            // identically. Applying them unconditionally also guarantees the importer is seen as
            // touched: when nothing else changes, Unity skips writing the meta and the sprite rects
            // assigned below are silently dropped.
            importer.textureType = TextureImporterType.Sprite;
            importer.spriteImportMode = SpriteImportMode.Multiple;
            importer.spritePixelsPerUnit = 16f;
            importer.filterMode = FilterMode.Point;
            importer.mipmapEnabled = false;
            importer.alphaIsTransparency = true;

            // UnityEditor.U2D.Sprites lives in the com.unity.2d.sprite package, which this project
            // does not depend on, so the core spritesheet API is the available path.
            List<SpriteMetaData> sheet = importer.spritesheet.ToList();
            int added = 0;
            foreach ((string name, int x, int y) in wanted)
            {
                Rect rect = new Rect(x, y, 16f, 16f);
                int index = sheet.FindIndex(entry => entry.name == name);
                SpriteMetaData data = new SpriteMetaData
                {
                    name = name,
                    rect = rect,
                    alignment = (int)SpriteAlignment.Center,
                    pivot = new Vector2(0.5f, 0.5f),
                    border = Vector4.zero
                };
                if (index >= 0)
                {
                    sheet[index] = data;
                }
                else
                {
                    sheet.Add(data);
                    added++;
                }
            }

            importer.spritesheet = sheet.ToArray();
            EditorUtility.SetDirty(importer);
            importer.SaveAndReimport();
            // SaveAndReimport can defer when the texture is already referenced by loaded assets,
            // and LoadAllAssetsAtPath would then miss the sprites added in this same session.
            AssetDatabase.ImportAsset(
                texturePath,
                ImportAssetOptions.ForceUpdate | ImportAssetOptions.ForceSynchronousImport);
            Debug.Log(
                $"ArenaBackdropBuilder sprites texture={texturePath} " +
                $"total={sheet.Count} added={added}");
        }

        private static Dictionary<string, TileBase> CreateTiles(
            string texturePath,
            Dictionary<string, string> assetNames)
        {
            Dictionary<string, Sprite> sprites = AssetDatabase
                .LoadAllAssetsAtPath(texturePath)
                .OfType<Sprite>()
                .ToDictionary(sprite => sprite.name, sprite => sprite);

            Dictionary<string, TileBase> tiles = new Dictionary<string, TileBase>();
            foreach ((string spriteName, string assetName) in assetNames)
            {
                if (!sprites.TryGetValue(spriteName, out Sprite sprite))
                {
                    throw new InvalidOperationException(
                        $"Sprite {spriteName} was not produced from {texturePath}");
                }

                string path = $"{TileFolder}/{assetName}.asset";
                Tile tile = AssetDatabase.LoadAssetAtPath<Tile>(path);
                if (tile == null)
                {
                    tile = ScriptableObject.CreateInstance<Tile>();
                    tile.sprite = sprite;
                    tile.colliderType = Tile.ColliderType.Sprite;
                    AssetDatabase.CreateAsset(tile, path);
                }
                else
                {
                    tile.sprite = sprite;
                    tile.colliderType = Tile.ColliderType.Sprite;
                    EditorUtility.SetDirty(tile);
                }
                tiles.Add(spriteName, tile);
            }
            Debug.Log($"ArenaBackdropBuilder prepared {tiles.Count} tiles from {texturePath}");
            return tiles;
        }

        private static void PaintBackdrop(Dictionary<string, TileBase> tiles)
        {
            TileBase grass = Load<Tile>(GrassTile);
            TileBase dirtA = tiles["ground.dirt-a"];
            TileBase dirtB = tiles["ground.dirt-b"];

            GameObject contents = PrefabUtility.LoadPrefabContents(BackdropPrefab);
            try
            {
                Tilemap ground = FindTilemap(contents, "Ground");
                Tilemap decoration = FindTilemap(contents, "Decoration");

                ground.ClearAllTiles();
                for (int x = GroundMinX; x <= GroundMaxX; x++)
                {
                    for (int y = GroundMinY; y <= GroundMaxY; y++)
                    {
                        bool inField = x >= PlayMin && x <= PlayMax && y >= PlayMin && y <= PlayMax;
                        TileBase tile = inField
                            ? (Scatter(x, y) ? dirtB : dirtA)
                            : grass;
                        ground.SetTile(new Vector3Int(x, y, 0), tile);
                    }
                }

                decoration.ClearAllTiles();
                decoration.SetTile(new Vector3Int(RingMin, RingMax, 0),
                    tiles["wall.brick.corner-top-left"]);
                decoration.SetTile(new Vector3Int(RingMax, RingMax, 0),
                    tiles["wall.brick.corner-top-right"]);
                decoration.SetTile(new Vector3Int(RingMin, RingMin, 0),
                    tiles["wall.brick.corner-bottom-left"]);
                decoration.SetTile(new Vector3Int(RingMax, RingMin, 0),
                    tiles["wall.brick.corner-bottom-right"]);
                for (int x = PlayMin; x <= PlayMax; x++)
                {
                    bool window = (x - PlayMin) % WindowEveryCells == 2;
                    decoration.SetTile(new Vector3Int(x, RingMax, 0),
                        window ? tiles["wall.brick.window"] : tiles["wall.brick.top"]);
                    decoration.SetTile(new Vector3Int(x, RingMin, 0),
                        tiles["wall.brick.bottom"]);
                }
                for (int y = PlayMin; y <= PlayMax; y++)
                {
                    decoration.SetTile(new Vector3Int(RingMin, y, 0), tiles["wall.brick.left"]);
                    decoration.SetTile(new Vector3Int(RingMax, y, 0), tiles["wall.brick.right"]);
                }

                ground.CompressBounds();
                decoration.CompressBounds();
                ClearProps(contents);

                PrefabUtility.SaveAsPrefabAsset(contents, BackdropPrefab);
                Debug.Log(
                    $"ArenaBackdropBuilder ground={ground.cellBounds} " +
                    $"decoration={decoration.cellBounds} " +
                    $"groundTiles={ground.GetUsedTilesCount()} " +
                    $"decorationTiles={decoration.GetUsedTilesCount()}");
            }
            finally
            {
                PrefabUtility.UnloadPrefabContents(contents);
            }
        }

        /// <summary>
        /// Deterministic sparse variation so 400 field cells do not read as one repeated stamp.
        /// A checkerboard would trade one visible pattern for another, so this scatters roughly a
        /// quarter of the cells using a cheap integer hash.
        /// </summary>
        private static bool Scatter(int x, int y)
        {
            int hash = x * 73856093 ^ y * 19349663;
            return (hash & 3) == 0;
        }

        private static void ClearProps(GameObject contents)
        {
            Transform stale = contents.transform.Find(PropsGroup);
            if (stale != null)
            {
                UnityEngine.Object.DestroyImmediate(stale.gameObject);
            }

            GameObject group = new GameObject(PropsGroup);
            group.transform.SetParent(contents.transform, false);
        }

        private static Tilemap FindTilemap(GameObject contents, string name)
        {
            Tilemap tilemap = contents.GetComponentsInChildren<Tilemap>(true)
                .SingleOrDefault(candidate => candidate.name == name);
            if (tilemap == null)
            {
                throw new InvalidOperationException($"Arena backdrop has no {name} Tilemap");
            }
            return tilemap;
        }

        private static T Load<T>(string path) where T : UnityEngine.Object
        {
            T asset = AssetDatabase.LoadAssetAtPath<T>(path);
            if (asset == null)
            {
                throw new InvalidOperationException($"Missing asset {path}");
            }
            return asset;
        }
    }
}
