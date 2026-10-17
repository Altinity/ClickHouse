# Conditional bulk delete support

**User:** Anonymous  
**Created:** 9/25/2026 22:28:04  
**Updated:** 9/25/2026 22:30:44  
**Exported:** 9/26/2026 14:17:11  
**Link:** [https://chatgpt.com/c/6ab6d952-c174-83eb-944d-bd3ca6c0250e](https://chatgpt.com/c/6ab6d952-c174-83eb-944d-bd3ca6c0250e)  

## Prompt:
9/25/2026, 10:29:36 PM

aws s3, GCS, и Azure поддерживают conditional delete (gcs через generation), aws s3 через If-Match при удалении одного объекта. Поддерживают ли они то же самое при bulk delete?

## Response:
9/25/2026, 10:29:44 PM

Проверю именно **атомарные/условные preconditions на каждый объект внутри bulk/batch delete**, потому что у трёх облаков “bulk delete” устроен по-разному.

> **Проверяю bulk-удаление**
>
> Я уточняю официальные возможности AWS S3, Google Cloud Storage и Azure Blob Storage: поддерживают ли их массовые операции удаления индивидуальные проверки версии или ETag для каждого объекта.
>
> **Проверил массовое удаление**
>
> 
>
> **Уточняю массовое удаление**
>
> Я перепроверяю нюанс: AWS S3, GCS и Azure могут поддерживать условия на уровне отдельных операций внутри bulk-запроса, но SDK и конкретные типы массовых API могут заметно различаться.
>
> **Уточнил массовое удаление**
>
> 
>
> **Comparing bulk deletion**
>
> I’m distinguishing true multi-delete APIs from batched individual requests. AWS supports per-object ETags directly; GCS and Azure generally preserve conditional headers within each nested delete, with independent, non-atomic results.
>
> **Compared bulk deletion options**
>
> 
>
> **Формулирую вывод**
>
> Я свожу различия: во всех трех случаях условия возможны, но AWS принимает ETag прямо для каждого объекта, а GCS и Azure упаковывают отдельные условные DELETE-запросы; результат независим для каждого объекта.
>
> **Formulated a conclusion**
>
> 
>
> **Searched 3 websites**
>
> 
>
> Worked for 47s

Да. На текущий момент **все три - S3, GCS и Azure Blob Storage - позволяют сделать conditional delete для каждого объекта внутри bulk/batch-запроса**, но API устроены немного по-разному.

| Storage | Conditional bulk delete | Как задаётся condition | Поведение при mismatch |
|---|---|---|---|
| **AWS S3** | ✅ | `ETag` внутри каждого `<Object>` в `DeleteObjects` | ошибка только для конкретного объекта |
| **GCS** | ✅ | `ifGenerationMatch` в каждом вложенном `DELETE` batch request | ошибка только для конкретного subrequest |
| **Azure Blob** | ✅ | `If-Match` / `BlobRequestConditions` в каждом `Delete Blob` subrequest | ошибка только для конкретного subrequest |

### AWS S3

Это теперь прямо поддерживается `DeleteObjects`. То есть `If-Match` для одиночного `DELETE` превращается в **per-object `ETag` в body** multi-delete:

```xml
<Delete>
  <Object>
    <Key>a.parquet</Key>
    <ETag>etag-a</ETag>
  </Object>
  <Object>
    <Key>b.parquet</Key>
    <ETag>etag-b</ETag>
  </Object>
</Delete>
```

AWS явно документирует conditional delete через `DeleteObjects` для обычных general-purpose buckets: если ETag совпал - объект попадает в `<Deleted>`, если нет - конкретный объект попадает в `<Error>`. Остальные объекты при этом могут успешно удалиться. ([AWS Documentation](https://docs.aws.amazon.com/AmazonS3/latest/userguide/conditional-deletes.html?utm_source=chatgpt.com))

`DeleteObjects` принимает до **1000 объектов** за запрос. ([AWS Documentation](https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteObjects.html))

Для S3 Express / directory buckets возможностей даже больше: кроме ETag можно указывать per-object `LastModifiedTime` и `Size`. ([AWS Documentation](https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteObjects.html))

То есть семантически:

```text
delete [
    {key: A, if_etag: X},
    {key: B, if_etag: Y},
    {key: C, if_etag: Z}
]
```

поддерживается нативно.

### Google Cloud Storage

Да, но модель другая: у GCS нет одного `DeleteObjects` API наподобие S3. Их batch - это `multipart/mixed`, содержащий несколько обычных JSON API запросов.

Каждый вложенный запрос является полноценным отдельным HTTP request и обрабатывается как обычный request. ([Google Cloud Documentation](https://docs.cloud.google.com/storage/docs/batch?hl=en)) Поэтому можно сделать:

```http
DELETE /storage/v1/b/bucket/o/a?ifGenerationMatch=123

DELETE /storage/v1/b/bucket/o/b?ifGenerationMatch=456

DELETE /storage/v1/b/bucket/o/c?ifGenerationMatch=789
```

`objects.delete` официально поддерживает `ifGenerationMatch`. ([Google Cloud Documentation](https://docs.cloud.google.com/storage/docs/json_api/v1/objects/delete?utm_source=chatgpt.com))

И это также экспонировано в SDK. Например Java `StorageBatch.delete()` принимает `BlobSourceOption[]`, включая `generationMatch`. ([Google Cloud Documentation](https://docs.cloud.google.com/java/docs/reference/google-cloud-storage/latest/com.google.cloud.storage.StorageBatch?utm_source=chatgpt.com))

GCS рекомендует держать batch в пределах **100 subrequests**. Они считаются отдельными операциями и фактически разбираются сервером на N независимых запросов. ([Google Cloud Documentation](https://docs.cloud.google.com/storage/docs/batch?hl=en))

### Azure Blob Storage

Тоже да. `Blob Batch` представляет собой multipart-запрос из отдельных `Delete Blob` subrequests. Каждый выполняется независимо; batch не атомарный. Максимум **256 subrequests**. ([Microsoft Learn](https://learn.microsoft.com/en-us/rest/api/storageservices/blob-batch))

Поэтому можно примерно так:

```http
DELETE /container/a HTTP/1.1
If-Match: "etag-a"

DELETE /container/b HTTP/1.1
If-Match: "etag-b"
```

Сам `Delete Blob` поддерживает conditional headers, включая `If-Match`, `If-Unmodified-Since` и другие. ([Microsoft Learn](https://learn.microsoft.com/rest/api/storageservices/delete-blob?utm_source=chatgpt.com))

Причём Azure SDK явно это поддерживает для batch. В .NET есть:

```csharp
BlobBatch.DeleteBlob(
    Uri blobUri,
    DeleteSnapshotsOption snapshotsOption,
    BlobRequestConditions conditions
)
```

то есть `BlobRequestConditions.IfMatch = etag` можно указать **отдельно для каждого blob**, после чего отправить весь `BlobBatch`. ([Microsoft Learn](https://learn.microsoft.com/en-us/dotnet/api/azure.storage.blobs.specialized.blobbatch.deleteblob?view=azure-dotnet))

Есть небольшой SDK-нюанс: shortcut `BlobBatchClient.DeleteBlobsAsync(IEnumerable<Uri> ...)` conditions не принимает. Для conditional batch надо собрать `BlobBatch` вручную через `DeleteBlob(..., BlobRequestConditions)`. ([Microsoft Learn](https://learn.microsoft.com/en-us/dotnet/api/azure.storage.blobs.specialized.blobbatchclient.deleteblobsasync?view=azure-dotnet&utm_source=chatgpt.com))

Итого для реализации optimistic/concurrent-safe GC:

```text
AWS:
  DeleteObjects([{key, etag}, ...])

GCS:
  Batch([
    DELETE key1?ifGenerationMatch=g1,
    DELETE key2?ifGenerationMatch=g2,
    ...
  ])

Azure:
  BlobBatch([
    DELETE key1 + If-Match: etag1,
    DELETE key2 + If-Match: etag2,
    ...
  ])
```

Во всех трёх случаях важная семантика одна и та же: **это не транзакционный all-or-nothing batch**. Condition проверяется per object; объект, изменившийся между list/head и delete, не удаляется, а остальные могут быть удалены. Для реализации GC/object cleanup это как раз обычно нужная семантика.

---

**Sources:**

- [How to perform conditional deletes - Amazon Simple Storage Service](https://docs.aws.amazon.com/AmazonS3/latest/userguide/conditional-deletes.html?utm_source=chatgpt.com)
- [Sending batch requests  |  Cloud Storage  |  Google Cloud Documentation](https://docs.cloud.google.com/storage/docs/batch?hl=en)
- [Blob Batch (REST API) - Azure Storage | Microsoft Learn](https://learn.microsoft.com/en-us/rest/api/storageservices/blob-batch)



---
Powered by [ChatGPT Exporter](https://www.chatgptexporter.com)