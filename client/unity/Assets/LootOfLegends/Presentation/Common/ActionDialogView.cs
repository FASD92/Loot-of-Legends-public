using System;
using UnityEngine;
using UnityEngine.UI;

namespace LootOfLegends.Presentation.Common
{
    public sealed class ActionDialogView : MonoBehaviour
    {
        private Text message;
        private Button confirmButton;
        private Button cancelButton;
        private Action confirmAction;
        private Action cancelAction;

        public bool IsVisible => gameObject.activeSelf;
        public string Message => message.text;
        public string ConfirmCopy => confirmButton.GetComponentInChildren<Text>(true).text;

        public static ActionDialogView Create(
            Transform parent,
            Image dialogStyle,
            Text textTemplate,
            Button buttonTemplate)
        {
            if (parent == null || textTemplate == null || buttonTemplate == null)
            {
                return null;
            }

            var root = new GameObject(
                "ActionDialog",
                typeof(RectTransform),
                typeof(CanvasRenderer),
                typeof(Image),
                typeof(ActionDialogView));
            root.layer = parent.gameObject.layer;
            RectTransform rootRect = root.GetComponent<RectTransform>();
            rootRect.SetParent(parent, false);
            Stretch(rootRect);
            Image dim = root.GetComponent<Image>();
            dim.color = new Color(0f, 0f, 0f, 0.55f);
            dim.raycastTarget = true;

            var dialog = new GameObject(
                "ActionDialogPanel",
                typeof(RectTransform),
                typeof(CanvasRenderer),
                typeof(Image));
            dialog.layer = root.layer;
            RectTransform dialogRect = dialog.GetComponent<RectTransform>();
            dialogRect.SetParent(rootRect, false);
            dialogRect.anchorMin = dialogRect.anchorMax = new Vector2(0.5f, 0.5f);
            dialogRect.sizeDelta = new Vector2(560f, 210f);
            Image dialogImage = dialog.GetComponent<Image>();
            if (dialogStyle != null)
            {
                dialogImage.sprite = dialogStyle.sprite;
                dialogImage.type = dialogStyle.type;
                dialogImage.color = dialogStyle.color;
            }

            Text message = Instantiate(textTemplate, dialogRect, false);
            message.name = "ActionDialogMessage";
            message.text = string.Empty;
            message.alignment = TextAnchor.MiddleCenter;
            SetRect(message.rectTransform, new Vector2(0f, 35f), new Vector2(500f, 70f));

            Button confirm = CloneButton(buttonTemplate, dialogRect, "ActionConfirmButton");
            Button cancel = CloneButton(buttonTemplate, dialogRect, "ActionCancelButton");
            ActionDialogView view = root.GetComponent<ActionDialogView>();
            view.message = message;
            view.confirmButton = confirm;
            view.cancelButton = cancel;
            confirm.onClick.AddListener(view.Confirm);
            cancel.onClick.AddListener(view.Cancel);
            view.Hide();
            return view;
        }

        public void ShowConfirmation(
            string copy,
            string confirmCopy,
            Action onConfirm,
            string cancelCopy,
            Action onCancel)
        {
            Show(copy, confirmCopy, onConfirm);
            cancelAction = onCancel;
            cancelButton.gameObject.SetActive(true);
            SetButtonLabel(cancelButton, cancelCopy);
            SetRect(confirmButton.GetComponent<RectTransform>(),
                new Vector2(-130f, -55f), new Vector2(210f, 58f));
            SetRect(cancelButton.GetComponent<RectTransform>(),
                new Vector2(130f, -55f), new Vector2(210f, 58f));
        }

        public void ShowNotice(string copy)
        {
            Show(copy, "확인", null);
            cancelButton.gameObject.SetActive(false);
            SetRect(confirmButton.GetComponent<RectTransform>(),
                new Vector2(0f, -55f), new Vector2(210f, 58f));
        }

        public void Confirm()
        {
            Action action = confirmAction;
            Hide();
            action?.Invoke();
        }

        public void Cancel()
        {
            Action action = cancelAction;
            Hide();
            action?.Invoke();
        }

        public static void SetButtonLabel(Button button, string copy, int fontSize = 0)
        {
            Text label = button?.GetComponentInChildren<Text>(true);
            if (label == null)
            {
                return;
            }
            label.text = copy;
            label.color = Color.white;
            label.alignment = TextAnchor.MiddleCenter;
            label.raycastTarget = false;
            if (fontSize > 0)
            {
                label.fontSize = fontSize;
            }
            Stretch(label.rectTransform);
        }

        private void Show(string copy, string buttonCopy, Action onConfirm)
        {
            message.text = copy ?? string.Empty;
            confirmAction = onConfirm;
            cancelAction = null;
            confirmButton.gameObject.SetActive(true);
            confirmButton.interactable = true;
            SetButtonLabel(confirmButton, buttonCopy);
            gameObject.SetActive(true);
            transform.SetAsLastSibling();
        }

        private void Hide()
        {
            gameObject.SetActive(false);
            confirmAction = null;
            cancelAction = null;
        }

        private static Button CloneButton(Button template, Transform parent, string name)
        {
            Button button = Instantiate(template, parent, false);
            button.name = name;
            button.onClick = new Button.ButtonClickedEvent();
            button.gameObject.SetActive(true);
            return button;
        }

        private static void Stretch(RectTransform rect)
        {
            rect.anchorMin = Vector2.zero;
            rect.anchorMax = Vector2.one;
            rect.offsetMin = Vector2.zero;
            rect.offsetMax = Vector2.zero;
        }

        private static void SetRect(RectTransform rect, Vector2 position, Vector2 size)
        {
            rect.anchorMin = rect.anchorMax = new Vector2(0.5f, 0.5f);
            rect.anchoredPosition = position;
            rect.sizeDelta = size;
        }
    }
}
