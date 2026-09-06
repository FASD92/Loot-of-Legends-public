using UnityEngine;

namespace LootOfLegends.Presentation.Arena
{
    [RequireComponent(typeof(SpriteRenderer))]
    public sealed class ArenaPlayerVisual : MonoBehaviour
    {
        private const float FramesPerSecond = 6f;

        private const int NicknameFontSize = 48;
        private const float NicknameCharacterSize = 0.075f;

        // 촬영은 1920x1080 이고 아레나 카메라는 world unit 당 50px 이므로 0.02 가 정확히 1px 이다.
        private const float NicknameOutlineOffset = 0.02f;

        private const int NicknameSortingOrder = 30;

        // 밝은 바닥(#90775E) 위 진한 갈색 단색 글씨는 대비 2.54:1, 어두운 캐릭터 스프라이트
        // 위에서는 1.63:1 이라 사실상 안 읽혔다. 외곽선을 두르면 글자 경계 대비가 16.98:1 로
        // 고정되어 뒤에 바닥이 오든 스프라이트가 오든 판독된다.
        private static readonly Color32 NicknameFillColor =
            new Color32(0xFF, 0xF3, 0xD6, 0xFF);

        private static readonly Color32 NicknameOutlineColor =
            new Color32(0x1A, 0x10, 0x08, 0xFF);

        // 8 방향이라 대각 모서리까지 막힌다. 4 방향만 쓰면 글자 대각선에 구멍이 남는다.
        private static readonly Vector2[] NicknameOutlineDirections =
        {
            new Vector2(-1f, 0f),
            new Vector2(1f, 0f),
            new Vector2(0f, -1f),
            new Vector2(0f, 1f),
            new Vector2(-1f, -1f),
            new Vector2(-1f, 1f),
            new Vector2(1f, -1f),
            new Vector2(1f, 1f)
        };

        private SpriteRenderer spriteRenderer;
        private ArenaPlayerAppearanceSet appearances;
        private int appearanceSlot;
        private ArenaFacing facing = ArenaFacing.Down;
        private int frame;
        private float elapsed;
        private bool hasPosition;
        private bool moving;
        private TextMesh nicknameLabel;
        private TextMesh[] nicknameOutline;

        public void Bind(ArenaPlayerAppearanceSet set, int appearanceSlot)
        {
            appearances = set != null
                ? set
                : throw new System.ArgumentNullException(nameof(set));
            appearances.Resolve(appearanceSlot, ArenaFacing.Down, 0);
            this.appearanceSlot = appearanceSlot;
            spriteRenderer = GetComponent<SpriteRenderer>();
            facing = ArenaFacing.Down;
            frame = 0;
            elapsed = 0f;
            hasPosition = false;
            moving = false;
            RenderFrame();
        }

        public void Project(Vector3 serverPosition)
        {
            EnsureBound();
            if (!hasPosition)
            {
                transform.localPosition = serverPosition;
                hasPosition = true;
                RenderFrame();
                return;
            }

            Vector3 delta = serverPosition - transform.localPosition;
            moving = delta.x != 0f || delta.y != 0f;
            if (moving)
            {
                ArenaFacing next = FacingFor(delta);
                if (next != facing)
                {
                    facing = next;
                    frame = 0;
                    elapsed = 0f;
                }
            }
            else
            {
                frame = 0;
                elapsed = 0f;
            }

            transform.localPosition = serverPosition;
            RenderFrame();
        }

        public void SetNickname(string nickname)
        {
            if (nicknameLabel == null)
            {
                nicknameLabel = CreateNicknameMesh(
                    transform,
                    "Nickname",
                    NicknameFillColor,
                    NicknameSortingOrder);
                nicknameLabel.transform.localPosition = new Vector3(0f, 0.9f, -0.1f);

                // 외곽선을 본체의 자식으로 둔다. GetComponentInChildren<TextMesh> 는 깊이
                // 우선이라 본체를 먼저 방문하므로, 외곽선을 더해도 호출자가 받는 것은
                // 여전히 본체다.
                nicknameOutline = new TextMesh[NicknameOutlineDirections.Length];
                for (int index = 0; index < nicknameOutline.Length; index++)
                {
                    TextMesh outline = CreateNicknameMesh(
                        nicknameLabel.transform,
                        "NicknameOutline" + index,
                        NicknameOutlineColor,
                        NicknameSortingOrder - 1);
                    Vector2 direction = NicknameOutlineDirections[index];
                    outline.transform.localPosition = new Vector3(
                        direction.x * NicknameOutlineOffset,
                        direction.y * NicknameOutlineOffset,
                        0f);
                    nicknameOutline[index] = outline;
                }
            }

            string copy = nickname ?? string.Empty;
            nicknameLabel.text = copy;
            foreach (TextMesh outline in nicknameOutline)
            {
                outline.text = copy;
            }
        }

        private static TextMesh CreateNicknameMesh(
            Transform parent,
            string name,
            Color32 color,
            int sortingOrder)
        {
            var host = new GameObject(name);
            host.transform.SetParent(parent, false);
            var mesh = host.AddComponent<TextMesh>();
            mesh.anchor = TextAnchor.MiddleCenter;
            mesh.alignment = TextAlignment.Center;
            mesh.fontSize = NicknameFontSize;
            mesh.characterSize = NicknameCharacterSize;
            mesh.fontStyle = FontStyle.Bold;
            mesh.color = color;
            mesh.GetComponent<MeshRenderer>().sortingOrder = sortingOrder;
            return mesh;
        }

        private void Update()
        {
            if (!moving || appearances == null)
            {
                return;
            }

            elapsed += Time.deltaTime;
            int advances = Mathf.FloorToInt(elapsed * FramesPerSecond);
            if (advances < 1)
            {
                return;
            }
            frame = (frame + advances) % 4;
            elapsed -= advances / FramesPerSecond;
            RenderFrame();
        }

        private static ArenaFacing FacingFor(Vector3 delta)
        {
            if (delta.x < 0f)
            {
                return ArenaFacing.Left;
            }
            if (delta.x > 0f)
            {
                return ArenaFacing.Right;
            }
            return delta.y > 0f ? ArenaFacing.Up : ArenaFacing.Down;
        }

        private void EnsureBound()
        {
            if (appearances == null)
            {
                throw new System.InvalidOperationException(
                    "Arena player visual is not bound");
            }
        }

        private void RenderFrame()
        {
            spriteRenderer.sprite = appearances.Resolve(
                appearanceSlot,
                facing,
                frame);
        }
    }
}
