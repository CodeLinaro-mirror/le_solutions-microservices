# Annotation


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the file citation. Always &#x60;file_citation&#x60;. | [default to 'file_citation']
**file_id** | **str** | The ID of the file.  | 
**index** | **int** | The index of the file in the list of files.  | 
**url** | **str** | The URL of the web resource. | 
**start_index** | **int** | The index of the first character of the URL citation in the message. | 
**end_index** | **int** | The index of the last character of the URL citation in the message. | 
**title** | **str** | The title of the web resource. | 

## Example

```python
from openapi_client.models.annotation import Annotation

# TODO update the JSON string below
json = "{}"
# create an instance of Annotation from a JSON string
annotation_instance = Annotation.from_json(json)
# print the JSON string representation of the object
print(Annotation.to_json())

# convert the object into a dict
annotation_dict = annotation_instance.to_dict()
# create an instance of Annotation from a dict
annotation_from_dict = Annotation.from_dict(annotation_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


